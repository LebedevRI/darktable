/*
    This file is part of darktable,
    copyright (c) 2010-2011 Henrik Andersson. (splittoning iop)
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
#include <lcms2.h>
#include "bauhaus/bauhaus.h"
#include "common/colorspaces.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "control/control.h"
#include "dtgtk/resetlabel.h"
#include "dtgtk/gradientslider.h"
#include "dtgtk/button.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "gui/presets.h"
#include <gtk/gtk.h>
#include <inttypes.h>

#define CLIP(x) ((x < 0) ? 0.0 : (x > 1.0) ? 1.0 : x)
DT_MODULE_INTROSPECTION(1, dt_iop_lplittoning_params_t)

typedef struct dt_iop_lplittoning_params_t
{
  float shadow_hue;
  float shadow_saturation;
  float highlight_hue;
  float highlight_saturation;
  float balance;  // center luminance of gradient
  float compress; // Compress range
} dt_iop_lplittoning_params_t;

typedef struct dt_iop_lplittoning_gui_data_t
{
  GtkWidget *highlight_hue, *highlight_saturation;
  GtkWidget *colorpick_highlight;
  GtkWidget *shadow_hue, *shadow_saturation;
  GtkWidget *colorpick_shadow;
  GtkWidget *balance, *compress;
} dt_iop_lplittoning_gui_data_t;

typedef struct dt_iop_lplittoning_data_t
{
  float shadow_Lab[3];
  float highlight_Lab[3];
  float balance;  // center luminance of gradient}
  float compress; // Compress range
} dt_iop_lplittoning_data_t;

const char *name()
{
  return _("split toning (Lab)");
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
  dt_accel_register_iop(self, FALSE, NC_("accel", "pick shadow color"), 0, 0);
  dt_accel_register_iop(self, FALSE, NC_("accel", "pick highlight color"), 0, 0);

  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "balance"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "compress"));
}

void connect_key_accels(dt_iop_module_t *self)
{
  dt_iop_lplittoning_gui_data_t *g = (dt_iop_lplittoning_gui_data_t *)self->gui_data;

  dt_accel_connect_button_iop(self, "pick shadow color", g->colorpick_shadow);
  dt_accel_connect_button_iop(self, "pick highlight color", g->colorpick_highlight);

  dt_accel_connect_slider_iop(self, "balance", g->balance);
  dt_accel_connect_slider_iop(self, "compress", g->compress);
}

void init_presets(dt_iop_module_so_t *self)
{
  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), "begin", NULL, NULL, NULL);

  // shadows: #ED7212
  // highlights: #ECA413
  // balance : 63
  // compress : 0
  dt_gui_presets_add_generic(
      _("authentic sepia"), self->op, self->version(),
      &(dt_iop_lplittoning_params_t){ 26.0 / 360.0, 92.0 / 100.0, 40.0 / 360.0, 92.0 / 100.0, 63.0, 0.0 },
      sizeof(dt_iop_lplittoning_params_t), 1);

  // shadows: #446CBB
  // highlights: #446CBB
  // balance : 0
  // compress : 5.22
  dt_gui_presets_add_generic(
      _("authentic cyanotype"), self->op, self->version(),
      &(dt_iop_lplittoning_params_t){ 220.0 / 360.0, 64.0 / 100.0, 220.0 / 360.0, 64.0 / 100.0, 0.0, 5.22 },
      sizeof(dt_iop_lplittoning_params_t), 1);

  // shadows : #A16C5E
  // highlights : #A16C5E
  // balance : 100
  // compress : 0
  dt_gui_presets_add_generic(
      _("authentic platinotype"), self->op, self->version(),
      &(dt_iop_lplittoning_params_t){ 13.0 / 360.0, 42.0 / 100.0, 13.0 / 360.0, 42.0 / 100.0, 100.0, 0.0 },
      sizeof(dt_iop_lplittoning_params_t), 1);

  // shadows: #211A14
  // highlights: #D9D0C7
  // balance : 60
  // compress : 0
  dt_gui_presets_add_generic(
      _("chocolate brown"), self->op, self->version(),
      &(dt_iop_lplittoning_params_t){ 28.0 / 360.0, 39.0 / 100.0, 28.0 / 360.0, 8.0 / 100.0, 60.0, 0.0 },
      sizeof(dt_iop_lplittoning_params_t), 1);

  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), "commit", NULL, NULL, NULL);
}

void process(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid, void *ovoid,
             const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  const dt_iop_lplittoning_data_t *const data = (dt_iop_lplittoning_data_t *)piece->data;

  const int ch = piece->colors;

  const float compress = data->compress / 2.0f;
#ifdef _OPENMP
#pragma omp parallel for default(none) shared(ovoid) schedule(static)
#endif
  for(int k = 0; k < roi_out->height; k++)
  {
    float *in = ((float *)ivoid) + (size_t)ch * k * roi_out->width;
    float *out = ((float *)ovoid) + (size_t)ch * k * roi_out->width;

    for(int j = 0; j < roi_out->width; j++, in += ch, out += ch)
    {
      if(in[0] < data->balance - compress || in[0] > data->balance + compress)
      {
        const float a = in[0] < data->balance ? data->shadow_Lab[1] : data->highlight_Lab[1];
        const float b = in[0] < data->balance ? data->shadow_Lab[2] : data->highlight_Lab[2];

        const double ra = in[0] < data->balance
                              ? CLIP((fabs(-data->balance + compress + in[0]) * 2.0 / 100.f))
                              : CLIP((fabs(-data->balance - compress + in[0]) * 2.0 / 100.f));

        out[0] = in[0];
        out[1] = in[1] * (1.0 - ra) + a * ra;
        out[2] = in[2] * (1.0 - ra) + b * ra;
      }
      else
      {
        out[0] = in[0];
        out[1] = in[1];
        out[2] = in[2];
      }

      out[3] = in[3];
    }
  }
}

static void balance_callback(GtkWidget *slider, dt_iop_module_t *self)
{
  if(self->dt->gui->reset) return;
  dt_iop_lplittoning_params_t *p = (dt_iop_lplittoning_params_t *)self->params;
  p->balance = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void compress_callback(GtkWidget *slider, dt_iop_module_t *self)
{
  if(self->dt->gui->reset) return;
  dt_iop_lplittoning_params_t *p = (dt_iop_lplittoning_params_t *)self->params;
  p->compress = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static inline void update_colorpicker_color(GtkWidget *colorpicker, float hue, float sat)
{
  float rgb[3];
  hsl2rgb(rgb, hue, sat, 0.5);

  GdkRGBA color = (GdkRGBA){.red = rgb[0], .green = rgb[1], .blue = rgb[2], .alpha = 1.0 };
  gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(colorpicker), &color);
}

static inline void update_saturation_slider_end_color(GtkWidget *slider, float hue)
{
  float rgb[3];
  hsl2rgb(rgb, hue, 1.0, 0.5);
  dt_bauhaus_slider_set_stop(slider, 1.0, rgb[0], rgb[1], rgb[2]);
}

static inline void update_balance_slider_colors(GtkWidget *slider, float hue1, float hue2)
{
  float rgb[3];
  if(hue1 != -1)
  {
    hsl2rgb(rgb, hue1, 1.0, 0.5);
    dt_bauhaus_slider_set_stop(slider, 0.0, rgb[0], rgb[1], rgb[2]);
  }
  if(hue2 != -1)
  {
    hsl2rgb(rgb, hue2, 1.0, 0.5);
    dt_bauhaus_slider_set_stop(slider, 1.0, rgb[0], rgb[1], rgb[2]);
  }
}

static void hue_callback(GtkWidget *slider, dt_iop_module_t *self)
{
  dt_iop_lplittoning_params_t *p = (dt_iop_lplittoning_params_t *)self->params;
  dt_iop_lplittoning_gui_data_t *g = (dt_iop_lplittoning_gui_data_t *)self->gui_data;

  double hue = 0;
  double saturation = 0;
  GtkWidget *colorpicker;
  GtkWidget *sat_slider = NULL;
  if(slider == g->highlight_hue)
  {
    // Shadows
    hue = p->shadow_hue = dt_bauhaus_slider_get(slider);
    saturation = p->shadow_saturation;
    colorpicker = g->colorpick_shadow;
    sat_slider = g->highlight_saturation;
    update_balance_slider_colors(g->balance, -1, hue);
  }
  else
  {
    hue = p->highlight_hue = dt_bauhaus_slider_get(slider);
    saturation = p->highlight_saturation;
    colorpicker = g->colorpick_highlight;
    sat_slider = g->shadow_saturation;
    update_balance_slider_colors(g->balance, hue, -1);
  }

  update_colorpicker_color(colorpicker, hue, saturation);
  update_saturation_slider_end_color(sat_slider, hue);

  if(self->dt->gui->reset) return;

  gtk_widget_queue_draw(sat_slider);

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void saturation_callback(GtkWidget *slider, dt_iop_module_t *self)
{
  dt_iop_lplittoning_params_t *p = (dt_iop_lplittoning_params_t *)self->params;
  dt_iop_lplittoning_gui_data_t *g = (dt_iop_lplittoning_gui_data_t *)self->gui_data;

  double hue = 0;
  double saturation = 0;
  GtkWidget *colorpicker;
  if(slider == g->highlight_saturation)
  {
    // Shadows
    hue = dt_bauhaus_slider_get(g->highlight_hue);
    p->shadow_saturation = saturation = dt_bauhaus_slider_get(slider);
    colorpicker = g->colorpick_shadow;
  }
  else
  {
    hue = dt_bauhaus_slider_get(g->shadow_hue);
    p->highlight_saturation = saturation = dt_bauhaus_slider_get(slider);
    colorpicker = g->colorpick_highlight;
  }

  update_colorpicker_color(colorpicker, hue, saturation);

  if(self->dt->gui->reset) return;
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void colorpick_callback(GtkColorButton *widget, dt_iop_module_t *self)
{
  if(self->dt->gui->reset) return;

  dt_iop_lplittoning_gui_data_t *g = (dt_iop_lplittoning_gui_data_t *)self->gui_data;

  float color[3], h, s, l;

  GdkRGBA c;
  gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(widget), &c);
  color[0] = c.red;
  color[1] = c.green;
  color[2] = c.blue;
  rgb2hsl(color, &h, &s, &l);

  dt_bauhaus_slider_set((GTK_WIDGET(widget) == g->colorpick_shadow) ? g->highlight_hue : g->shadow_hue, h);
  dt_bauhaus_slider_set(
      (GTK_WIDGET(widget) == g->colorpick_shadow) ? g->highlight_saturation : g->shadow_saturation, s);

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

void commit_params(dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_lplittoning_params_t *p = (dt_iop_lplittoning_params_t *)p1;
  dt_iop_lplittoning_data_t *d = (dt_iop_lplittoning_data_t *)piece->data;

  cmsHPROFILE RGB = dt_colorspaces_create_srgb_profile();
  cmsHPROFILE Lab = dt_colorspaces_create_lab_profile();

  cmsHTRANSFORM xform = cmsCreateTransform(RGB, TYPE_RGB_FLT, Lab, TYPE_Lab_FLT, INTENT_PERCEPTUAL, 0);

  float shadow_rgb[3];
  hsl2rgb(shadow_rgb, p->shadow_hue, p->shadow_saturation, 0.5);
  cmsDoTransform(xform, shadow_rgb, d->shadow_Lab, 1);

  float highlight_rgb[3];
  hsl2rgb(highlight_rgb, p->highlight_hue, p->highlight_saturation, 0.5);
  cmsDoTransform(xform, highlight_rgb, d->highlight_Lab, 1);

  cmsDeleteTransform(xform);
  dt_colorspaces_cleanup_profile(Lab);
  dt_colorspaces_cleanup_profile(RGB);

  d->balance = p->balance;
  d->compress = p->compress;
}

void init_pipe(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = calloc(1, sizeof(dt_iop_lplittoning_data_t));
  self->commit_params(self, self->default_params, pipe, piece);
}

void cleanup_pipe(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
  piece->data = NULL;
}

void gui_update(dt_iop_module_t *self)
{
  dt_iop_lplittoning_gui_data_t *g = (dt_iop_lplittoning_gui_data_t *)self->gui_data;
  dt_iop_lplittoning_params_t *p = (dt_iop_lplittoning_params_t *)self->params;

  dt_bauhaus_slider_set(g->highlight_hue, p->shadow_hue);
  dt_bauhaus_slider_set(g->shadow_hue, p->highlight_hue);
  dt_bauhaus_slider_set(g->shadow_saturation, p->highlight_saturation);
  dt_bauhaus_slider_set(g->highlight_saturation, p->shadow_saturation);
  dt_bauhaus_slider_set(g->balance, p->balance);
  dt_bauhaus_slider_set(g->compress, p->compress);

  update_colorpicker_color(g->colorpick_shadow, p->shadow_hue, p->shadow_saturation);
  update_colorpicker_color(g->colorpick_highlight, p->highlight_hue, p->highlight_saturation);
  update_saturation_slider_end_color(g->highlight_saturation, p->shadow_hue);
  update_saturation_slider_end_color(g->shadow_saturation, p->highlight_hue);

  update_balance_slider_colors(g->balance, p->highlight_hue, p->shadow_hue);
}

void init(dt_iop_module_t *self)
{
  self->params = malloc(sizeof(dt_iop_lplittoning_params_t));
  self->default_params = malloc(sizeof(dt_iop_lplittoning_params_t));
  self->default_enabled = 0;

  // just before colorin
  self->priority = 808; // module order created by iop_dependencies.py, do not edit!
  self->params_size = sizeof(dt_iop_lplittoning_params_t);
  self->gui_data = NULL;
  dt_iop_lplittoning_params_t tmp = (dt_iop_lplittoning_params_t){ 0, 0.5, 0.2, 0.5, 50.0, 33.0 };
  memcpy(self->params, &tmp, sizeof(dt_iop_lplittoning_params_t));
  memcpy(self->default_params, &tmp, sizeof(dt_iop_lplittoning_params_t));
}

void cleanup(dt_iop_module_t *self)
{
  free(self->gui_data);
  self->gui_data = NULL;
  free(self->params);
  self->params = NULL;
}

static inline void gui_init_tab(dt_iop_module_t *self, const char *name, GtkWidget **ppcolor,
                                const GdkRGBA *c, GtkWidget **pphue, GtkWidget **ppsaturation)
{
  gtk_box_pack_start(GTK_BOX(self->widget), dt_ui_section_label_new(name), FALSE, FALSE, 5);

  // color button
  GtkWidget *color;
  *ppcolor = color = gtk_color_button_new_with_rgba(c);
  gtk_widget_set_size_request(color, DT_PIXEL_APPLY_DPI(32), DT_PIXEL_APPLY_DPI(32));
  gtk_color_chooser_set_use_alpha(GTK_COLOR_CHOOSER(color), FALSE);
  gtk_color_button_set_title(GTK_COLOR_BUTTON(color), _("select tone color"));

  // hue slider
  GtkWidget *hue;
  *pphue = hue = (dt_bauhaus_slider_new_with_range_and_feedback(self, 0.0f, 1.0f, 0.01f, 0.0f, 2, 0));
  dt_bauhaus_slider_set_stop(hue, 0.0f, 1.0f, 0.0f, 0.0f);
  dt_bauhaus_widget_set_label(hue, NULL, _("hue"));
  dt_bauhaus_slider_set_stop(hue, 0.166f, 1.0f, 1.0f, 0.0f);
  dt_bauhaus_slider_set_stop(hue, 0.322f, 0.0f, 1.0f, 0.0f);
  dt_bauhaus_slider_set_stop(hue, 0.498f, 0.0f, 1.0f, 1.0f);
  dt_bauhaus_slider_set_stop(hue, 0.664f, 0.0f, 0.0f, 1.0f);
  dt_bauhaus_slider_set_stop(hue, 0.830f, 1.0f, 0.0f, 1.0f);
  dt_bauhaus_slider_set_stop(hue, 1.0f, 1.0f, 0.0f, 0.0f);
  g_object_set(G_OBJECT(hue), "tooltip-text", _("select the hue tone"), (char *)NULL);

  // saturation slider
  GtkWidget *saturation;
  *ppsaturation = saturation = dt_bauhaus_slider_new_with_range(self, 0.0f, 1.0f, 0.01f, 0.0f, 2);
  dt_bauhaus_widget_set_label(saturation, NULL, _("saturation"));
  dt_bauhaus_slider_set_stop(saturation, 0.0f, 0.2f, 0.2f, 0.2f);
  dt_bauhaus_slider_set_stop(saturation, 1.0f, 1.0f, 1.0f, 1.0f);
  g_object_set(G_OBJECT(saturation), "tooltip-text", _("select the saturation tone"), (char *)NULL);

  // pack the widgets
  GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);
  gtk_box_pack_start(GTK_BOX(vbox), hue, FALSE, TRUE, 0);
  gtk_box_pack_end(GTK_BOX(vbox), saturation, FALSE, TRUE, 0);

  GtkWidget *hbox = GTK_WIDGET(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0));
  gtk_box_pack_start(GTK_BOX(hbox), vbox, TRUE, TRUE, 0);
  gtk_box_pack_end(GTK_BOX(hbox), color, FALSE, FALSE, 0);

  gtk_box_pack_start(GTK_BOX(self->widget), hbox, TRUE, TRUE, 0);
}

void gui_init(dt_iop_module_t *self)
{
  self->gui_data = malloc(sizeof(dt_iop_lplittoning_gui_data_t));
  dt_iop_lplittoning_gui_data_t *g = (dt_iop_lplittoning_gui_data_t *)self->gui_data;
  dt_iop_lplittoning_params_t *p = (dt_iop_lplittoning_params_t *)self->params;

  self->widget = GTK_WIDGET(gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE));

  float rgb[3];

  // Shadows
  hsl2rgb(rgb, p->shadow_hue, p->shadow_saturation, 0.5f);
  GdkRGBA sh_color = (GdkRGBA){.red = rgb[0], .green = rgb[1], .blue = rgb[2], .alpha = 1.0 };
  gui_init_tab(self, _("shadows"), &g->colorpick_shadow, &sh_color, &g->highlight_hue,
               &g->highlight_saturation);

  // Highlights
  hsl2rgb(rgb, p->highlight_hue, p->highlight_saturation, 0.5f);
  GdkRGBA hi_color = (GdkRGBA){.red = rgb[0], .green = rgb[1], .blue = rgb[2], .alpha = 1.0 };
  gui_init_tab(self, _("highlights"), &g->colorpick_highlight, &hi_color, &g->shadow_hue,
               &g->shadow_saturation);

  // Additional parameters
  GtkWidget *hbox = GTK_WIDGET(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0));
  GtkWidget *vbox = GTK_WIDGET(gtk_box_new(GTK_ORIENTATION_VERTICAL, 0));
  gtk_box_pack_start(GTK_BOX(hbox), vbox, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), hbox, TRUE, TRUE, 0);

  g->balance = dt_bauhaus_slider_new_with_range_and_feedback(self, 0.0, 100.0, 0.1, p->balance, 2, 0);
  dt_bauhaus_slider_set_format(g->balance, "%.2f");
  dt_bauhaus_slider_set_stop(g->balance, 0.0f, 0.5f, 0.5f, 0.5f);
  dt_bauhaus_slider_set_stop(g->balance, 1.0f, 0.5f, 0.5f, 0.5f);
  dt_bauhaus_widget_set_label(g->balance, NULL, _("balance"));
  gtk_box_pack_start(GTK_BOX(vbox), g->balance, TRUE, TRUE, 0);

  g->compress = dt_bauhaus_slider_new_with_range(self, 0.0, 100.0, 1.0, p->compress, 2);
  dt_bauhaus_slider_set_format(g->compress, "%.2f%%");
  dt_bauhaus_widget_set_label(g->compress, NULL, _("compress"));
  gtk_box_pack_start(GTK_BOX(vbox), g->compress, TRUE, TRUE, 0);

  g_object_set(G_OBJECT(g->balance), "tooltip-text", _("the balance of center of splittoning"), (char *)NULL);
  g_object_set(G_OBJECT(g->compress), "tooltip-text",
               _("compress the effect on highlights/shadows and\npreserve midtones"), (char *)NULL);

  g_signal_connect(G_OBJECT(g->highlight_hue), "value-changed", G_CALLBACK(hue_callback), self);
  g_signal_connect(G_OBJECT(g->shadow_hue), "value-changed", G_CALLBACK(hue_callback), self);

  g_signal_connect(G_OBJECT(g->highlight_saturation), "value-changed", G_CALLBACK(saturation_callback), self);
  g_signal_connect(G_OBJECT(g->shadow_saturation), "value-changed", G_CALLBACK(saturation_callback), self);

  g_signal_connect(G_OBJECT(g->balance), "value-changed", G_CALLBACK(balance_callback), self);
  g_signal_connect(G_OBJECT(g->compress), "value-changed", G_CALLBACK(compress_callback), self);

  g_signal_connect(G_OBJECT(g->colorpick_shadow), "color-set", G_CALLBACK(colorpick_callback), self);
  g_signal_connect(G_OBJECT(g->colorpick_highlight), "color-set", G_CALLBACK(colorpick_callback), self);
}

void gui_cleanup(dt_iop_module_t *self)
{
  free(self->gui_data);
  self->gui_data = NULL;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
