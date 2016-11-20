/*
 *    This file is part of darktable,
 *    copyright (c) 2016 Roman Lebedev
 *
 *    (based on noiseprofiles.h, copyright (c) 2015 tobias ellinghaus.)
 *
 *    darktable is free software: you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation, either version 3 of the License, or
 *    (at your option) any later version.
 *
 *    darktable is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *
 *    You should have received a copy of the GNU General Public License
 *    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "common/wb_presets.h"
#include "common/darktable.h"     // for dt_print, dt_debug_thread_t::DT_DE...
#include "common/file_location.h" // for dt_loc_get_datadir
#include <glib-object.h>          // for g_object_unref
#include <glib/gi18n.h>           // for C_
#include <limits.h>               // for PATH_MAX
#include <stdio.h>                // for NULL, snprintf, fprintf, stderr
#include <stdlib.h>               // for free, malloc

JsonParser *dt_wb_presets_init(const char *alternative)
{
  GError *error = NULL;
  char filename[PATH_MAX] = { 0 };

  if(alternative == NULL)
  {
    // TODO: shall we look for profiles in the user config dir?
    char datadir[PATH_MAX] = { 0 };
    dt_loc_get_datadir(datadir, sizeof(datadir));
    snprintf(filename, sizeof(filename), "%s/%s", datadir, "wb_presets.json");
  }
  else
    snprintf(filename, sizeof(filename), "%s", alternative);

  dt_print(DT_DEBUG_CONTROL, "[wb_presets] loading wb_presets from `%s'\n", filename);
  if(!g_file_test(filename, G_FILE_TEST_EXISTS)) return NULL;

  // TODO: shall we cache the content? for now this looks fast enough(TM)
  JsonParser *parser = json_parser_new();
  if(!json_parser_load_from_file(parser, filename, &error))
  {
    fprintf(stderr, "[wb_presets] error: parsing json from `%s' failed\n%s\n", filename, error->message);
    g_error_free(error);
    g_object_unref(parser);
    return NULL;
  }

  return parser;
}

static gint _sort_by_tuning(gconstpointer a, gconstpointer b)
{
  const dt_wb_preset_t *profile_a = a;
  const dt_wb_preset_t *profile_b = b;

  return profile_a->tuning - profile_b->tuning;
}

GList *dt_wb_presets_get_matching(const dt_image_t *cimg)
{
  JsonParser *parser = darktable.wb_presets_parser;
  JsonReader *reader = NULL;
  GList *result = NULL;

  if(!parser) goto end;

  dt_print(DT_DEBUG_CONTROL, "[wb_presets] looking for maker `%s', model `%s'\n", cimg->camera_maker,
           cimg->camera_model);

  JsonNode *root = json_parser_get_root(parser);

  reader = json_reader_new(root);

  json_reader_read_member(reader, "wb_presets");

  // go through all makers
  const unsigned int n_makers = json_reader_count_elements(reader);
  dt_print(DT_DEBUG_CONTROL, "[wb_presets] found %d makers\n", n_makers);
  for(unsigned int i = 0; i < n_makers; i++)
  {
    json_reader_read_element(reader, i);

    json_reader_read_member(reader, "maker");

    if(g_strstr_len(cimg->camera_maker, -1, json_reader_get_string_value(reader)))
    {
      dt_print(DT_DEBUG_CONTROL, "[wb_presets] found `%s' as `%s'\n", cimg->camera_maker,
               json_reader_get_string_value(reader));

      // go through all models and check those
      json_reader_end_member(reader);

      json_reader_read_member(reader, "models");

      const unsigned int n_models = json_reader_count_elements(reader);
      dt_print(DT_DEBUG_CONTROL, "[wb_presets] found %d models\n", n_models);
      for(unsigned int j = 0; j < n_models; j++)
      {
        json_reader_read_element(reader, j);

        json_reader_read_member(reader, "model");

        if(!g_strcmp0(cimg->camera_model, json_reader_get_string_value(reader)))
        {
          dt_print(DT_DEBUG_CONTROL, "[wb_presets] found %s\n", cimg->camera_model);

          // we got a match, return at most bufsize elements
          json_reader_end_member(reader);

          json_reader_read_member(reader, "presets");

          const unsigned int n_presets = json_reader_count_elements(reader);
          dt_print(DT_DEBUG_CONTROL, "[wb_presets] found %d presets\n", n_presets);
          for(unsigned int k = 0; k < n_presets; k++)
          {
            json_reader_read_element(reader, k);

            gchar *name = NULL;

            // name
            if(json_reader_read_member(reader, "name"))
            {
              name = g_strdup(json_reader_get_string_value(reader));
              json_reader_end_member(reader);
            }
            else
            {
              json_reader_read_member(reader, "temperature");
              name = g_strdup_printf("%li%s", json_reader_get_int_value(reader), C_("temperature", "K"));
              json_reader_end_member(reader);
            }

            json_reader_read_member(reader, "tunings");

            const unsigned int n_tunings = json_reader_count_elements(reader);
            dt_print(DT_DEBUG_CONTROL, "[wb_presets] found %d tunings\n", n_tunings);
            for(unsigned int h = 0; h < n_tunings; h++)
            {
              dt_wb_preset_t tmp_profile = { 0 };

              json_reader_read_element(reader, h);

              // maker
              tmp_profile.maker = g_strdup(cimg->camera_maker);

              // model
              tmp_profile.model = g_strdup(cimg->camera_model);

              // name
              tmp_profile.name = g_strdup(name);

              // tuning
              json_reader_read_member(reader, "tuning");
              tmp_profile.tuning = (int8_t)json_reader_get_int_value(reader);
              json_reader_end_member(reader);

              // coeffients
              json_reader_read_member(reader, "coeffients");
              for(unsigned int c = 0; c < 4; c++)
              {
                json_reader_read_element(reader, c);
                tmp_profile.channel[c] = json_reader_get_double_value(reader);
                json_reader_end_element(reader);
              }
              json_reader_end_member(reader);

              json_reader_end_element(reader);

              // everything worked out, add tmp_profile to result
              dt_wb_preset_t *new_profile = malloc(sizeof(dt_wb_preset_t));
              *new_profile = tmp_profile;
              result = g_list_append(result, new_profile);
            } // tunings

            g_free(name);

            json_reader_end_member(reader);
            json_reader_end_element(reader);
          } // presets

          goto end;
        }

        json_reader_end_member(reader);
        json_reader_end_element(reader);
      } // models
    }

    json_reader_end_member(reader);
    json_reader_end_element(reader);
  } // makers

  json_reader_end_member(reader);

end:
  if(reader) g_object_unref(reader);
  if(result) result = g_list_sort(result, _sort_by_tuning);
  return result;
}

void dt_wb_preset_free(gpointer data)
{
  dt_wb_preset_t *profile = data;
  g_free(profile->maker);
  g_free(profile->model);
  g_free(profile->name);
  free(profile);
}

void dt_wb_preset_interpolate(const dt_wb_preset_t *const p1, const dt_wb_preset_t *const p2, dt_wb_preset_t *out)
{
  const double t = CLAMP((double)(out->tuning - p1->tuning) / (double)(p2->tuning - p1->tuning), 0.0, 1.0);
  for(int k = 0; k < 3; k++)
  {
    out->channel[k] = 1.0 / (((1.0 - t) / p1->channel[k]) + (t / p2->channel[k]));
  }
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
