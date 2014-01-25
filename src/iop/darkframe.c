/*
    This file is part of darktable,
    copyright (c) 2009--2012 johannes hanika.

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
#include "dtgtk/button.h"
#include "dtgtk/paint.h"

#include <gtk/gtk.h>
#include <stdlib.h>
#include <xmmintrin.h>

DT_MODULE_INTROSPECTION(1, dt_iop_darkframe_params_t)

typedef struct dt_iop_darkframe_params_t
{
  char filename[PATH_MAX];
  int divider;
}
dt_iop_darkframe_params_t;

typedef struct dt_iop_darkframe_gui_data_t
{
  GtkEntry *entry;
  GtkWidget *button;
}
dt_iop_darkframe_gui_data_t;

const char *name()
{
  return _("dark frame");
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
  if(!dt_dev_pixelpipe_uses_downsampled_input(pipe) && piece->pipe->image.filters && piece->pipe->image.bpp != 4) return sizeof(uint16_t);
  if(!dt_dev_pixelpipe_uses_downsampled_input(pipe) && piece->pipe->image.filters && piece->pipe->image.bpp == 4) return sizeof(float);
  return 4*sizeof(float);
}

void process (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *ivoid, void *ovoid, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  dt_iop_darkframe_params_t *d = (dt_iop_darkframe_params_t *)piece->data;

  for(int k=0; k<3; k++)
    piece->pipe->processed_maximum[k] = piece->pipe->processed_maximum[k] / d->divider;
}

/** optional: if this exists, it will be called to init new defaults if a new image is loaded from film strip mode. */
void reload_defaults(dt_iop_module_t *module)
{
  dt_iop_darkframe_params_t tmp = (dt_iop_darkframe_params_t)
  {
    ""
  };

  memcpy(module->params, &tmp, sizeof(dt_iop_darkframe_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_darkframe_params_t));
}

/** init, cleanup, commit to pipeline */
void init(dt_iop_module_t *module)
{
  // we don't need global data:
  module->params = malloc(sizeof(dt_iop_darkframe_params_t));
  module->default_params = malloc(sizeof(dt_iop_darkframe_params_t));
  module->default_enabled = 0;
  module->priority = 16; // module order created by iop_dependencies.py, do not edit!
  module->params_size = sizeof(dt_iop_darkframe_params_t);
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
  dt_iop_darkframe_gui_data_t *g = (dt_iop_darkframe_gui_data_t *)self->gui_data;
  dt_iop_darkframe_params_t *p = (dt_iop_darkframe_params_t *)self->params;

  gtk_entry_set_text(GTK_ENTRY(g->entry), p->filename);
}

static void
entry_callback (GtkWidget *widget, gpointer *user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_darkframe_gui_data_t *g = (dt_iop_darkframe_gui_data_t *)self->gui_data;
  dt_iop_darkframe_params_t *p = (dt_iop_darkframe_params_t *)self->params;

  g_strlcpy(p->filename, gtk_entry_get_text(GTK_ENTRY(g->entry)), PATH_MAX);

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void
button_clicked (GtkWidget *widget, gpointer *user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_darkframe_gui_data_t *g = (dt_iop_darkframe_gui_data_t *)self->gui_data;
  dt_iop_darkframe_params_t *p = (dt_iop_darkframe_params_t *)self->params;
  GtkWidget *win = dt_ui_main_window(darktable.gui->ui);
  GtkWidget *filechooser = gtk_file_chooser_dialog_new (_("select dark frame"),
                           GTK_WINDOW (win),
                           GTK_FILE_CHOOSER_ACTION_OPEN,
                           GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
                           GTK_STOCK_OPEN, GTK_RESPONSE_ACCEPT,
                           (char *)NULL);

  gtk_file_chooser_set_select_multiple(GTK_FILE_CHOOSER(filechooser), FALSE);
  gchar *old = g_strdup(gtk_entry_get_text(g->entry));
  char *c = g_strstr_len(old, -1, "$");
  if(c) *c = '\0';
  gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(filechooser), old);
  g_free(old);
  if (gtk_dialog_run (GTK_DIALOG (filechooser)) == GTK_RESPONSE_ACCEPT)
  {
    gchar *filename = gtk_file_chooser_get_filename (GTK_FILE_CHOOSER (filechooser));
    gtk_entry_set_text(GTK_ENTRY(g->entry), filename);
    g_strlcpy(p->filename, filename, PATH_MAX);
  }
  gtk_widget_destroy (filechooser);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

void gui_init(dt_iop_module_t *self)
{
  self->gui_data = malloc(sizeof(dt_iop_darkframe_gui_data_t));
  dt_iop_darkframe_gui_data_t *g = (dt_iop_darkframe_gui_data_t *)self->gui_data;

  self->widget = gtk_hbox_new(FALSE, 5);
  g->entry = GTK_ENTRY(gtk_entry_new());
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->entry), TRUE, TRUE, 0);
  dt_gui_key_accel_block_on_focus_connect (GTK_WIDGET (g->entry));
  g_object_set(G_OBJECT(g->entry), "tooltip-text", _("dark frame file"), (char *)NULL);
  g_signal_connect(G_OBJECT(g->entry), "changed", G_CALLBACK(entry_callback), self);

  g->button = dtgtk_button_new(dtgtk_cairo_paint_directory, 0);
  gtk_widget_set_size_request(g->button, 18, 18);
  g_object_set(G_OBJECT(g->button), "tooltip-text", _("select dark frame"), (char *)NULL);
  gtk_box_pack_start(GTK_BOX(self->widget), g->button, FALSE, FALSE, 0);
  g_signal_connect(G_OBJECT(g->button), "clicked", G_CALLBACK(button_clicked), self);
}

void gui_cleanup(dt_iop_module_t *self)
{
  free(self->gui_data);
  self->gui_data = NULL;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
