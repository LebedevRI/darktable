/*
    This file is part of darktable,
    copyright (c) 2016 Roman Lebedev

    (based on noiseprofiles.h, copyright (c) 2015 tobias ellinghaus.)

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

#pragma once

#include "common/image.h"        // for dt_image_t
#include <glib.h>                // for GList, gpointer
#include <json-glib/json-glib.h> // for JsonParser
#include <stdint.h>              // for int8_t

typedef struct dt_wb_preset_t
{
  char *maker;
  char *model;
  char *name;
  int8_t tuning;
  double channel[4];
} dt_wb_preset_t;

/** read the wb_preset file once on startup (kind of) */
JsonParser *dt_wb_presets_init(const char *alternative);

/*
 * returns the dt_wb_preset_t matching the image's exif data.
 * free with g_list_free_full(..., dt_wb_preset_free);
 */
GList *dt_wb_presets_get_matching(const dt_image_t *cimg);

/** convenience function to free a list of dt_wb_preset_t */
void dt_wb_preset_free(gpointer data);

/*
 * interpolate values from p1 and p2 into out.
 */
void dt_wb_preset_interpolate(const dt_wb_preset_t *const p1, // the smaller tuning
                              const dt_wb_preset_t *const p2, // the larger tuning (can't be == tuning1)
                              dt_wb_preset_t *out);           // has tuning initialized

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
