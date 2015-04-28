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
#include "gui/gtk.h"
#include "gui/presets.h"
#include <gtk/gtk.h>
#include <inttypes.h>

#define DT_COLORRECONSTRUCT_BILATERAL_MAX_RES_S 500
#define DT_COLORRECONSTRUCT_BILATERAL_MAX_RES_R 100
#define DT_COLORRECONSTRUCT_SPATIAL_APPROX 100.0f

DT_MODULE_INTROSPECTION(1, dt_iop_highlightsinpaint_params_t)

typedef struct dt_iop_highlightsinpaint_params1_t
{
  float threshold;
  float spatial;
  float range;
} dt_iop_highlightsinpaint_params1_t;

typedef struct dt_iop_highlightsinpaint_params2_t
{
  float threshold;
  float spatial;
  float range;
} dt_iop_highlightsinpaint_params2_t;

typedef struct dt_iop_highlightsinpaint_params_t
{
  float threshold;
  float spatial;
  float range;
} dt_iop_highlightsinpaint_params_t;

typedef struct dt_iop_highlightsinpaint_RGB_t
{
  float R;
  float G;
  float B;
  float weight;
} dt_iop_highlightsinpaint_RGB_t;

typedef struct dt_iop_highlightsinpaint_bilateral_frozen_t
{
  size_t size_x, size_y, size_z;
  int width, height, x, y;
  float scale;
  float sigma_s, sigma_r;
  dt_iop_highlightsinpaint_RGB_t *buf;
} dt_iop_highlightsinpaint_bilateral_frozen_t;

typedef struct dt_iop_highlightsinpaint_gui_data_t
{
  GtkWidget *threshold;
  GtkWidget *spatial;
  GtkWidget *range;
  dt_iop_highlightsinpaint_bilateral_frozen_t *can;
  dt_pthread_mutex_t lock;
} dt_iop_highlightsinpaint_gui_data_t;

typedef struct dt_iop_highlightsinpaint_data_t
{
  float threshold;
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
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "spatial extent"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "range extent"));
}

void connect_key_accels(dt_iop_module_t *self)
{
  dt_iop_highlightsinpaint_gui_data_t *g = (dt_iop_highlightsinpaint_gui_data_t *)self->gui_data;

  dt_accel_connect_slider_iop(self, "threshold", GTK_WIDGET(g->threshold));
  dt_accel_connect_slider_iop(self, "spatial extent", GTK_WIDGET(g->spatial));
  dt_accel_connect_slider_iop(self, "range extent", GTK_WIDGET(g->range));
}

typedef struct dt_iop_highlightsinpaint_bilateral_t
{
  size_t size_x, size_y, size_z;
  int width, height, x, y;
  float scale;
  float sigma_s, sigma_r;
  dt_iop_highlightsinpaint_RGB_t *buf;
} dt_iop_highlightsinpaint_bilateral_t;

static inline void image_to_grid(const dt_iop_highlightsinpaint_bilateral_t *const b, const float i,
                                 const float j, const float R, const float G, const float B, float *x,
                                 float *y, float *z_R, float *z_G, float *z_B)
{
  *x = CLAMPS(i / b->sigma_s, 0, b->size_x - 1);
  *y = CLAMPS(j / b->sigma_s, 0, b->size_y - 1);
  *z_R = CLAMPS(R / b->sigma_r, 0, b->size_z - 1);
  *z_G = CLAMPS(G / b->sigma_r, 0, b->size_z - 1);
  *z_B = CLAMPS(B / b->sigma_r, 0, b->size_z - 1);
}

static inline void grid_rescale(const dt_iop_highlightsinpaint_bilateral_t *const b, const int i, const int j,
                                const dt_iop_roi_t *roi, const float scale, float *px, float *py)
{
  *px = (roi->x + i) * scale - b->x;
  *py = (roi->y + j) * scale - b->y;
}

static void dt_iop_highlightsinpaint_bilateral_dump(dt_iop_highlightsinpaint_bilateral_frozen_t *bf)
{
  if(!bf) return;
  dt_free_align(bf->buf);
  free(bf);
}

static void dt_iop_highlightsinpaint_bilateral_free(dt_iop_highlightsinpaint_bilateral_t *b)
{
  if(!b) return;
  dt_free_align(b->buf);
  free(b);
}

static dt_iop_highlightsinpaint_bilateral_t *
dt_iop_highlightsinpaint_bilateral_init(const dt_iop_roi_t *roi, // dimensions of input image
                                        const float iscale,      // overall scale of input image
                                        const float sigma_s,     // spatial sigma (blur pixel coords)
                                        const float sigma_r)     // range sigma (blur luma values)
{
  dt_iop_highlightsinpaint_bilateral_t *b
      = (dt_iop_highlightsinpaint_bilateral_t *)malloc(sizeof(dt_iop_highlightsinpaint_bilateral_t));
  if(!b) return NULL;
  float _x = roundf(roi->width / sigma_s);
  float _y = roundf(roi->height / sigma_s);
  float _z = roundf(100.0f / sigma_r);
  b->size_x = CLAMPS((int)_x, 4, DT_COLORRECONSTRUCT_BILATERAL_MAX_RES_S) + 1;
  b->size_y = CLAMPS((int)_y, 4, DT_COLORRECONSTRUCT_BILATERAL_MAX_RES_S) + 1;
  b->size_z = CLAMPS((int)_z, 4, DT_COLORRECONSTRUCT_BILATERAL_MAX_RES_R) + 1;
  b->width = roi->width;
  b->height = roi->height;
  b->x = roi->x;
  b->y = roi->y;
  b->scale = iscale / roi->scale;
  b->sigma_s = MAX(roi->height / (b->size_y - 1.0f), roi->width / (b->size_x - 1.0f));
  b->sigma_r = 100.0f / (b->size_z - 1.0f);
  b->buf = dt_alloc_align(16, b->size_x * b->size_y * b->size_z * sizeof(dt_iop_highlightsinpaint_RGB_t));

  memset(b->buf, 0, b->size_x * b->size_y * b->size_z * sizeof(dt_iop_highlightsinpaint_RGB_t));
#if 0
  fprintf(stderr, "[bilateral] created grid [%d %d %d]"
          " with sigma (%f %f) (%f %f)\n", b->size_x, b->size_y, b->size_z,
          b->sigma_s, sigma_s, b->sigma_r, sigma_r);
#endif
  return b;
}

static dt_iop_highlightsinpaint_bilateral_frozen_t *
dt_iop_highlightsinpaint_bilateral_freeze(dt_iop_highlightsinpaint_bilateral_t *b)
{
  if(!b) return NULL;

  dt_iop_highlightsinpaint_bilateral_frozen_t *bf = (dt_iop_highlightsinpaint_bilateral_frozen_t *)malloc(
      sizeof(dt_iop_highlightsinpaint_bilateral_frozen_t));
  if(!bf) return NULL;

  bf->size_x = b->size_x;
  bf->size_y = b->size_y;
  bf->size_z = b->size_z;
  bf->width = b->width;
  bf->height = b->height;
  bf->x = b->x;
  bf->y = b->y;
  bf->scale = b->scale;
  bf->sigma_s = b->sigma_s;
  bf->sigma_r = b->sigma_r;
  bf->buf = dt_alloc_align(16, b->size_x * b->size_y * b->size_z * sizeof(dt_iop_highlightsinpaint_RGB_t));
  if(bf->buf && b->buf)
  {
    memcpy(bf->buf, b->buf, b->size_x * b->size_y * b->size_z * sizeof(dt_iop_highlightsinpaint_RGB_t));
  }
  else
  {
    dt_iop_highlightsinpaint_bilateral_dump(bf);
    return NULL;
  }

  return bf;
}

static dt_iop_highlightsinpaint_bilateral_t *
dt_iop_highlightsinpaint_bilateral_thaw(dt_iop_highlightsinpaint_bilateral_frozen_t *bf)
{
  if(!bf) return NULL;

  dt_iop_highlightsinpaint_bilateral_t *b
      = (dt_iop_highlightsinpaint_bilateral_t *)malloc(sizeof(dt_iop_highlightsinpaint_bilateral_t));
  if(!b) return NULL;

  b->size_x = bf->size_x;
  b->size_y = bf->size_y;
  b->size_z = bf->size_z;
  b->width = bf->width;
  b->height = bf->height;
  b->x = bf->x;
  b->y = bf->y;
  b->scale = bf->scale;
  b->sigma_s = bf->sigma_s;
  b->sigma_r = bf->sigma_r;
  b->buf = dt_alloc_align(16, b->size_x * b->size_y * b->size_z * sizeof(dt_iop_highlightsinpaint_RGB_t));
  if(b->buf && bf->buf)
  {
    memcpy(b->buf, bf->buf, b->size_x * b->size_y * b->size_z * sizeof(dt_iop_highlightsinpaint_RGB_t));
  }
  else
  {
    dt_iop_highlightsinpaint_bilateral_free(b);
    return NULL;
  }

  return b;
}


static void dt_iop_highlightsinpaint_bilateral_splat(const dt_iop_highlightsinpaint_bilateral_t *const b,
                                                     const float *const in, const float threshold)
{
// splat into downsampled grid
#ifdef _OPENMP
#pragma omp parallel for default(none)
#endif
  for(int j = 0; j < b->height; j++)
  {
    size_t index = 4 * j * b->width;
    for(int i = 0; i < b->width; i++, index += 4)
    {
      float x, y, z_R, z_G, z_B, weight;
      const float Rin = in[index];
      const float Gin = in[index + 1];
      const float Bin = in[index + 2];
      // we deliberately ignore pixels above threshold
      if(Rin > threshold || Gin > threshold || Bin > threshold) continue;

      weight = 1.0f;

      image_to_grid(b, i, j, Rin, Gin, Bin, &x, &y, &z_R, &z_G, &z_B);

      // closest integer splatting:
      const int xi = CLAMPS((int)round(x), 0, b->size_x - 1);
      const int yi = CLAMPS((int)round(y), 0, b->size_y - 1);
      const int zi = CLAMPS((int)round(z_R), 0, b->size_z - 1);
      // const int zi_R = CLAMPS((int)round(z_R), 0, b->size_z - 1);
      // const int zi_G = CLAMPS((int)round(z_G), 0, b->size_z - 1);
      // const int zi_B = CLAMPS((int)round(z_B), 0, b->size_z - 1);
      const size_t grid_index = xi + b->size_x * (yi + b->size_y * zi);

#ifdef _OPENMP
#pragma omp atomic
#endif
      b->buf[grid_index].R += Rin * weight;

#ifdef _OPENMP
#pragma omp atomic
#endif
      b->buf[grid_index].G += Gin * weight;

#ifdef _OPENMP
#pragma omp atomic
#endif
      b->buf[grid_index].B += Bin * weight;

#ifdef _OPENMP
#pragma omp atomic
#endif
      b->buf[grid_index].weight += weight;
    }
  }
}


static void blur_line(dt_iop_highlightsinpaint_RGB_t *buf, const int offset1, const int offset2,
                      const int offset3, const int size1, const int size2, const int size3)
{
  const float w0 = 6.f / 16.f;
  const float w1 = 4.f / 16.f;
  const float w2 = 1.f / 16.f;
#ifdef _OPENMP
#pragma omp parallel for default(none) shared(buf)
#endif
  for(int k = 0; k < size1; k++)
  {
    size_t index = (size_t)k * offset1;
    for(int j = 0; j < size2; j++)
    {
      dt_iop_highlightsinpaint_RGB_t tmp1 = buf[index];
      buf[index].R = buf[index].R * w0 + w1 * buf[index + offset3].R + w2 * buf[index + 2 * offset3].R;
      buf[index].G = buf[index].G * w0 + w1 * buf[index + offset3].G + w2 * buf[index + 2 * offset3].G;
      buf[index].B = buf[index].B * w0 + w1 * buf[index + offset3].B + w2 * buf[index + 2 * offset3].B;
      buf[index].weight = buf[index].weight * w0 + w1 * buf[index + offset3].weight
                          + w2 * buf[index + 2 * offset3].weight;
      index += offset3;
      dt_iop_highlightsinpaint_RGB_t tmp2 = buf[index];
      buf[index].R = buf[index].R * w0 + w1 * (buf[index + offset3].R + tmp1.R)
                     + w2 * buf[index + 2 * offset3].R;
      buf[index].G = buf[index].G * w0 + w1 * (buf[index + offset3].G + tmp1.G)
                     + w2 * buf[index + 2 * offset3].G;
      buf[index].B = buf[index].B * w0 + w1 * (buf[index + offset3].B + tmp1.B)
                     + w2 * buf[index + 2 * offset3].B;
      buf[index].weight = buf[index].weight * w0 + w1 * (buf[index + offset3].weight + tmp1.weight)
                          + w2 * buf[index + 2 * offset3].weight;
      index += offset3;
      for(int i = 2; i < size3 - 2; i++)
      {
        const dt_iop_highlightsinpaint_RGB_t tmp3 = buf[index];
        buf[index].R = buf[index].R * w0 + w1 * (buf[index + offset3].R + tmp2.R)
                       + w2 * (buf[index + 2 * offset3].R + tmp1.R);
        buf[index].G = buf[index].G * w0 + w1 * (buf[index + offset3].G + tmp2.G)
                       + w2 * (buf[index + 2 * offset3].G + tmp1.G);
        buf[index].B = buf[index].B * w0 + w1 * (buf[index + offset3].B + tmp2.B)
                       + w2 * (buf[index + 2 * offset3].B + tmp1.B);
        buf[index].weight = buf[index].weight * w0 + w1 * (buf[index + offset3].weight + tmp2.weight)
                            + w2 * (buf[index + 2 * offset3].weight + tmp1.weight);

        index += offset3;
        tmp1 = tmp2;
        tmp2 = tmp3;
      }
      const dt_iop_highlightsinpaint_RGB_t tmp3 = buf[index];
      buf[index].R = buf[index].R * w0 + w1 * (buf[index + offset3].R + tmp2.R) + w2 * tmp1.R;
      buf[index].G = buf[index].G * w0 + w1 * (buf[index + offset3].G + tmp2.G) + w2 * tmp1.G;
      buf[index].B = buf[index].B * w0 + w1 * (buf[index + offset3].B + tmp2.B) + w2 * tmp1.B;
      buf[index].weight = buf[index].weight * w0 + w1 * (buf[index + offset3].weight + tmp2.weight)
                          + w2 * tmp1.weight;
      index += offset3;
      buf[index].R = buf[index].R * w0 + w1 * tmp3.R + w2 * tmp2.R;
      buf[index].G = buf[index].G * w0 + w1 * tmp3.G + w2 * tmp2.G;
      buf[index].B = buf[index].B * w0 + w1 * tmp3.B + w2 * tmp2.B;
      buf[index].weight = buf[index].weight * w0 + w1 * tmp3.weight + w2 * tmp2.weight;
      index += offset3;
      index += offset2 - offset3 * size3;
    }
  }
}


static void dt_iop_highlightsinpaint_bilateral_blur(dt_iop_highlightsinpaint_bilateral_t *b)
{
  // gaussian up to 3 sigma
  blur_line(b->buf, b->size_x * b->size_y, b->size_x, 1, b->size_z, b->size_y, b->size_x);
  // gaussian up to 3 sigma
  blur_line(b->buf, b->size_x * b->size_y, 1, b->size_x, b->size_z, b->size_x, b->size_y);
  // gaussian up to 3 sigma
  blur_line(b->buf, 1, b->size_x, b->size_x * b->size_y, b->size_x, b->size_y, b->size_z);
}

static void dt_iop_highlightsinpaint_bilateral_slice(const dt_iop_highlightsinpaint_bilateral_t *const b,
                                                     const float *const in, float *out, const float threshold,
                                                     const dt_iop_roi_t *roi, const float iscale)
{
  const float rescale = iscale / (roi->scale * b->scale);
  const int ox = 1;
  const int oy = b->size_x;
  const int oz = b->size_y * b->size_x;
#ifdef _OPENMP
#pragma omp parallel for default(none) shared(out, roi)
#endif
  for(int j = 0; j < roi->height; j++)
  {
    size_t index = 4 * j * roi->width;
    for(int i = 0; i < roi->width; i++, index += 4)
    {
      float x, y, z_R, z_G, z_B;
      float px, py;
      const float Rin = out[index + 0] = in[index + 0];
      const float Gin = out[index + 1] = in[index + 1];
      const float Bin = out[index + 2] = in[index + 2];
      out[index + 3] = in[index + 3];
      const float blend = CLAMPS(20.0f / threshold * Rin - 19.0f, 0.0f, 1.0f);
      if(blend == 0.0f) continue;
      grid_rescale(b, i, j, roi, rescale, &px, &py);
      image_to_grid(b, px, py, Rin, Gin, Bin, &x, &y, &z_R, &z_G, &z_B);
      // trilinear lookup:
      const int xi = MIN((int)x, b->size_x - 2);
      const int yi = MIN((int)y, b->size_y - 2);
      const int zi = MIN((int)z_R, b->size_z - 2);
      const float xf = x - xi;
      const float yf = y - yi;
      const float zf = z_R - zi;
      const size_t gi = xi + b->size_x * (yi + b->size_y * zi);

      const float Rout = b->buf[gi].R * (1.0f - xf) * (1.0f - yf) * (1.0f - zf)
                         + b->buf[gi + ox].R * (xf) * (1.0f - yf) * (1.0f - zf)
                         + b->buf[gi + oy].R * (1.0f - xf) * (yf) * (1.0f - zf)
                         + b->buf[gi + ox + oy].R * (xf) * (yf) * (1.0f - zf)
                         + b->buf[gi + oz].R * (1.0f - xf) * (1.0f - yf) * (zf)
                         + b->buf[gi + ox + oz].R * (xf) * (1.0f - yf) * (zf)
                         + b->buf[gi + oy + oz].R * (1.0f - xf) * (yf) * (zf)
                         + b->buf[gi + ox + oy + oz].R * (xf) * (yf) * (zf);

      const float Gout = b->buf[gi].G * (1.0f - xf) * (1.0f - yf) * (1.0f - zf)
                         + b->buf[gi + ox].G * (xf) * (1.0f - yf) * (1.0f - zf)
                         + b->buf[gi + oy].G * (1.0f - xf) * (yf) * (1.0f - zf)
                         + b->buf[gi + ox + oy].G * (xf) * (yf) * (1.0f - zf)
                         + b->buf[gi + oz].G * (1.0f - xf) * (1.0f - yf) * (zf)
                         + b->buf[gi + ox + oz].G * (xf) * (1.0f - yf) * (zf)
                         + b->buf[gi + oy + oz].G * (1.0f - xf) * (yf) * (zf)
                         + b->buf[gi + ox + oy + oz].G * (xf) * (yf) * (zf);


      const float Bout = b->buf[gi].B * (1.0f - xf) * (1.0f - yf) * (1.0f - zf)
                         + b->buf[gi + ox].B * (xf) * (1.0f - yf) * (1.0f - zf)
                         + b->buf[gi + oy].B * (1.0f - xf) * (yf) * (1.0f - zf)
                         + b->buf[gi + ox + oy].B * (xf) * (yf) * (1.0f - zf)
                         + b->buf[gi + oz].B * (1.0f - xf) * (1.0f - yf) * (zf)
                         + b->buf[gi + ox + oz].B * (xf) * (1.0f - yf) * (zf)
                         + b->buf[gi + oy + oz].B * (1.0f - xf) * (yf) * (zf)
                         + b->buf[gi + ox + oy + oz].B * (xf) * (yf) * (zf);

      /*
            const float weight = b->buf[gi].weight * (1.0f - xf) * (1.0f - yf) * (1.0f - zf)
                                 + b->buf[gi + ox].weight * (xf) * (1.0f - yf) * (1.0f - zf)
                                 + b->buf[gi + oy].weight * (1.0f - xf) * (yf) * (1.0f - zf)
                                 + b->buf[gi + ox + oy].weight * (xf) * (yf) * (1.0f - zf)
                                 + b->buf[gi + oz].weight * (1.0f - xf) * (1.0f - yf) * (zf)
                                 + b->buf[gi + ox + oz].weight * (xf) * (1.0f - yf) * (zf)
                                 + b->buf[gi + oy + oz].weight * (1.0f - xf) * (yf) * (zf)
                                 + b->buf[gi + ox + oy + oz].weight * (xf) * (yf) * (zf);
      */
      out[index + 0] = Rout;
      out[index + 1] = Gout;
      out[index + 1] = Bout;
    }
  }
}


void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *ivoid, void *ovoid,
             const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  dt_iop_highlightsinpaint_data_t *data = (dt_iop_highlightsinpaint_data_t *)piece->data;
  dt_iop_highlightsinpaint_gui_data_t *g = (dt_iop_highlightsinpaint_gui_data_t *)self->gui_data;
  float *in = (float *)ivoid;
  float *out = (float *)ovoid;

  const float scale = piece->iscale / roi_in->scale;
  const float sigma_r = fmax(data->range, 0.1f);
  const float sigma_s = fmax(data->spatial, 1.0f) / scale;

  dt_iop_highlightsinpaint_bilateral_t *b;
  dt_iop_highlightsinpaint_bilateral_frozen_t *can = NULL;

  const float threshold
      = data->threshold * fminf(piece->pipe->processed_maximum[0],
                                fminf(piece->pipe->processed_maximum[1], piece->pipe->processed_maximum[2]));

  // color reconstruction often involves a massive spatial blur of the bilateral grid. this typically requires
  // more or less the whole image to contribute to the grid. In pixelpipe FULL we can not rely on this
  // as the pixelpipe might only see part of the image (region of interest). Therefore we "steal" the
  // bilateral grid
  // of the preview pipe if needed. However, the grid of the preview pipeline is coarser and may lead
  // to other artifacts so we only want to use it when necessary. The threshold for data->spatial has been
  // selected
  // arbitrarily.
  if(sigma_s > DT_COLORRECONSTRUCT_SPATIAL_APPROX && self->dev->gui_attached && g
     && piece->pipe->type == DT_DEV_PIXELPIPE_FULL)
  {
    // check how far we are zoomed-in
    dt_dev_zoom_t zoom = dt_control_get_dev_zoom();
    int closeup = dt_control_get_dev_closeup();
    const float min_scale = dt_dev_get_zoom_scale(self->dev, DT_ZOOM_FIT, closeup ? 2.0 : 1.0, 0);
    const float cur_scale = dt_dev_get_zoom_scale(self->dev, zoom, closeup ? 2.0 : 1.0, 0);

    dt_pthread_mutex_lock(&g->lock);
    // if we are zoomed in more than just a little bit, we try to use the canned grid of the preview pipeline
    can = (cur_scale > 1.05f * min_scale) ? g->can : NULL;
    dt_pthread_mutex_unlock(&g->lock);
  }

  if(can)
  {
    b = dt_iop_highlightsinpaint_bilateral_thaw(can);
  }
  else
  {
    b = dt_iop_highlightsinpaint_bilateral_init(roi_in, piece->iscale, sigma_s, sigma_r);
    dt_iop_highlightsinpaint_bilateral_splat(b, in, threshold);
    dt_iop_highlightsinpaint_bilateral_blur(b);
  }

  dt_iop_highlightsinpaint_bilateral_slice(b, in, out, threshold, roi_in, piece->iscale);

  // here is where we generate the canned bilateral grid of the preview pipe for later use
  if(self->dev->gui_attached && g && piece->pipe->type == DT_DEV_PIXELPIPE_PREVIEW)
  {
    dt_pthread_mutex_lock(&g->lock);
    dt_iop_highlightsinpaint_bilateral_dump(g->can);
    g->can = dt_iop_highlightsinpaint_bilateral_freeze(b);
    dt_pthread_mutex_unlock(&g->lock);
  }

  dt_iop_highlightsinpaint_bilateral_free(b);
}

static size_t
dt_iop_highlightsinpaint_bilateral_memory_use(const int width,     // width of input image
                                              const int height,    // height of input image
                                              const float sigma_s, // spatial sigma (blur pixel coords)
                                              const float sigma_r) // range sigma (blur luma values)
{
  float _x = roundf(width / sigma_s);
  float _y = roundf(height / sigma_s);
  float _z = roundf(100.0f / sigma_r);
  size_t size_x = CLAMPS((int)_x, 4, DT_COLORRECONSTRUCT_BILATERAL_MAX_RES_S) + 1;
  size_t size_y = CLAMPS((int)_y, 4, DT_COLORRECONSTRUCT_BILATERAL_MAX_RES_S) + 1;
  size_t size_z = CLAMPS((int)_z, 4, DT_COLORRECONSTRUCT_BILATERAL_MAX_RES_R) + 1;

  return size_x * size_y * size_z * sizeof(dt_iop_highlightsinpaint_RGB_t);
}


static size_t
dt_iop_highlightsinpaint_bilateral_singlebuffer_size(const int width,     // width of input image
                                                     const int height,    // height of input image
                                                     const float sigma_s, // spatial sigma (blur pixel coords)
                                                     const float sigma_r) // range sigma (blur luma values)
{
  float _x = roundf(width / sigma_s);
  float _y = roundf(height / sigma_s);
  float _z = roundf(100.0f / sigma_r);
  size_t size_x = CLAMPS((int)_x, 4, DT_COLORRECONSTRUCT_BILATERAL_MAX_RES_S) + 1;
  size_t size_y = CLAMPS((int)_y, 4, DT_COLORRECONSTRUCT_BILATERAL_MAX_RES_S) + 1;
  size_t size_z = CLAMPS((int)_z, 4, DT_COLORRECONSTRUCT_BILATERAL_MAX_RES_R) + 1;

  return size_x * size_y * size_z * sizeof(dt_iop_highlightsinpaint_RGB_t);
}

void tiling_callback(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece,
                     const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out,
                     struct dt_develop_tiling_t *tiling)
{
  dt_iop_highlightsinpaint_data_t *d = (dt_iop_highlightsinpaint_data_t *)piece->data;
  // the total scale is composed of scale before input to the pipeline (iscale),
  // and the scale of the roi.
  const float scale = piece->iscale / roi_in->scale;
  const float sigma_r = fmax(d->range, 0.1f);
  const float sigma_s = fmax(d->spatial, 1.0f) / scale;

  const int width = roi_in->width;
  const int height = roi_in->height;
  const int channels = piece->colors;

  const size_t basebuffer = width * height * channels * sizeof(float);

  tiling->factor = 2.0f
                   + (float)dt_iop_highlightsinpaint_bilateral_memory_use(width, height, sigma_s, sigma_r)
                         / basebuffer;
  tiling->maxbuf = fmax(1.0f, (float)dt_iop_highlightsinpaint_bilateral_singlebuffer_size(
                                  width, height, sigma_s, sigma_r) / basebuffer);
  tiling->overhead = 0;
  tiling->overlap = ceilf(4 * sigma_s);
  tiling->xalign = 1;
  tiling->yalign = 1;
  return;
}


static void threshold_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_highlightsinpaint_params_t *p = (dt_iop_highlightsinpaint_params_t *)self->params;
  p->threshold = dt_bauhaus_slider_get(slider);
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
  d->spatial = p->spatial;
  d->range = p->range;

#ifdef HAVE_OPENCL
  piece->process_cl_ready = (piece->process_cl_ready && !(darktable.opencl->avoid_atomics));
#endif
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
  dt_iop_highlightsinpaint_params_t tmp = (dt_iop_highlightsinpaint_params_t){ 1.0f, 400.0f, 10.0f };
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

  dt_pthread_mutex_init(&g->lock, NULL);
  g->can = NULL;

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);

  g->threshold = dt_bauhaus_slider_new_with_range(self, 0.0f, 2.0f, 0.01f, p->threshold, 2);
  g->spatial = dt_bauhaus_slider_new_with_range(self, 0.0f, 1000.0f, 1.0f, p->spatial, 2);
  g->range = dt_bauhaus_slider_new_with_range(self, 0.0f, 50.0f, 0.1f, p->range, 2);

  dt_bauhaus_widget_set_label(g->threshold, NULL, _("clipping threshold"));
  dt_bauhaus_widget_set_label(g->spatial, NULL, _("spatial extent"));
  dt_bauhaus_widget_set_label(g->range, NULL, _("range extent"));

  gtk_box_pack_start(GTK_BOX(self->widget), g->threshold, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), g->spatial, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), g->range, TRUE, TRUE, 0);

  g_object_set(g->threshold, "tooltip-text", _("manually adjust the clipping threshold against magenta "
                                               "highlights (you shouldn't ever need to touch this)"),
               (char *)NULL);
  g_object_set(g->spatial, "tooltip-text", _("how far to look for replacement colors in spatial dimensions"),
               (char *)NULL);
  g_object_set(g->range, "tooltip-text",
               _("how far to look for replacement colors in the luminance dimension"), (char *)NULL);

  g_signal_connect(G_OBJECT(g->threshold), "value-changed", G_CALLBACK(threshold_callback), self);
  g_signal_connect(G_OBJECT(g->spatial), "value-changed", G_CALLBACK(spatial_callback), self);
  g_signal_connect(G_OBJECT(g->range), "value-changed", G_CALLBACK(range_callback), self);
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  dt_iop_highlightsinpaint_gui_data_t *g = (dt_iop_highlightsinpaint_gui_data_t *)self->gui_data;
  dt_pthread_mutex_destroy(&g->lock);
  dt_iop_highlightsinpaint_bilateral_dump(g->can);
  free(self->gui_data);
  self->gui_data = NULL;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
