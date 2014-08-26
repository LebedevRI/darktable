/*
 *  This file is part of darktable,
 *  copyright (c) 2009--2013 johannes hanika.
 *  copyright (c) 2014 Ulrich Pegelow.
 *  copyright (c) 2014 LebedevRI.
 *
 *  darktable is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  darktable is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with darktable.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "common.h"

#include "interpolation.h"

void
backtransform(float2 *p, const float pixelAspectRatio)
{
  if(pixelAspectRatio < 1.0f)
  {
    (*p).y *= pixelAspectRatio;
  }
  else
  {
    (*p).x /= pixelAspectRatio;
  }
}

/* kernel for scale pixels: bilinear interpolation */
__kernel void
scalepixels_bilinear(read_only image2d_t in, write_only image2d_t out,
                     const int width, const int height,
                     const int in_width, const int in_height,
                     const float pixelAspectRatio)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  float2 po = {x, y};

  backtransform(&po, pixelAspectRatio);

  float4 pixel = interpolation_compute_pixel_bilinear_4f(in, in_width, in_height, po);

  write_imagef (out, (int2)(x, y), pixel);
}

/* kernel for scale pixels: bicubic interpolation */
__kernel void
scalepixels_bicubic(read_only image2d_t in, write_only image2d_t out,
                    const int width, const int height,
                    const int in_width, const int in_height,
                    const float pixelAspectRatio)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  float2 po = {x, y};

  backtransform(&po, pixelAspectRatio);

  float4 pixel = interpolation_compute_pixel_bicubic_4f(in, in_width, in_height, po);

  write_imagef (out, (int2)(x, y), pixel);
}

/* kernel for scale pixels: lanczos2 interpolation */
__kernel void
scalepixels_lanczos2(read_only image2d_t in, write_only image2d_t out,
                     const int width, const int height,
                     const int in_width, const int in_height,
                     const float pixelAspectRatio)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  float2 po = {x, y};

  backtransform(&po, pixelAspectRatio);

  float4 pixel = interpolation_compute_pixel_lanczos2_4f(in, in_width, in_height, po);

  write_imagef (out, (int2)(x, y), pixel);
}

/* kernel for scale pixels: lanczos3 interpolation */
__kernel void
scalepixels_lanczos3(read_only image2d_t in, write_only image2d_t out,
                     const int width, const int height,
                     const int in_width, const int in_height,
                     const float pixelAspectRatio)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  float2 po = {x, y};

  backtransform(&po, pixelAspectRatio);

  float4 pixel = interpolation_compute_pixel_lanczos3_4f(in, in_width, in_height, po);

  write_imagef (out, (int2)(x, y), pixel);
}
