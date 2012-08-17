/*
 * Copyright (c) 2011-2012 BalaBit IT Ltd, Budapest, Hungary
 * Copyright (c) 2011 Balint Kovacs <blint@balabit.hu>
 * Copyright (c) 2011-2012 Gergely Nagy <algernon@balabit.hu>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published
 * by the Free Software Foundation, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#include "plugin.h"
#include "templates.h"
#include "filter.h"
#include "filter-expr-parser.h"
#include "cfg.h"
#include "value-pairs.h"

#include <printbuf.h>
#include <json.h>
#include <json_object_private.h>

typedef struct _TFJsonState
{
  TFSimpleFuncState super;
  ValuePairs *vp;
} TFJsonState;

static gboolean
tf_json_prepare(LogTemplateFunction *self, gpointer s, LogTemplate *parent,
		gint argc, gchar *argv[],
		GError **error)
{
  TFJsonState *state = (TFJsonState *)s;

  state->vp = value_pairs_new_from_cmdline (parent->cfg, argc, argv, error);
  if (!state->vp)
    return FALSE;

  return TRUE;
}

static int
tf_json_object_to_string (struct json_object *jso,
			  struct printbuf *pb)
{
  int i = 0;
  struct json_object_iter iter;
  sprintbuf (pb, "{");

  json_object_object_foreachC (jso, iter)
    {
      gchar *esc;
      
      if (i)
	sprintbuf (pb, ",");
      sprintbuf (pb, "\"");
      esc = g_strescape (iter.key, NULL);
      sprintbuf (pb, esc);
      g_free (esc);
      sprintbuf (pb, "\":");
      if (iter.val == NULL)
	sprintbuf (pb, "null");
      else
	iter.val->_to_json_string (iter.val, pb);
      i++;
    }

  return sprintbuf(pb, "}");
}

static gboolean
tf_json_obj_start(const gchar *name,
                  const gchar *prefix, gpointer *prefix_data,
                  const gchar *prev, gpointer *prev_data,
                  gpointer user_data)
{
  struct json_object *o;

  if (prefix_data)
    {
      o = json_object_new_object();
      *prefix_data = o;
      o->_to_json_string = tf_json_object_to_string;
    }
  return FALSE;
}

static gboolean
tf_json_obj_end(const gchar *name,
                const gchar *prefix, gpointer *prefix_data,
                const gchar *prev, gpointer *prev_data,
                gpointer user_data)
{
  struct json_object *root;

  if (prev_data)
    root = (struct json_object *)*prev_data;
  else
    root = (struct json_object *)user_data;

  if (prefix_data)
    json_object_object_add (root, (gchar *) name, (struct json_object *)*prefix_data);
  return FALSE;
}

static gboolean
tf_json_value(const gchar *name, const gchar *prefix, const gchar *value,
              gpointer *prefix_data, gpointer user_data)
{
  struct json_object *root;
  struct json_object *this;

  if (prefix_data)
    root = (struct json_object *)*prefix_data;
  else
    root = (struct json_object *)user_data;

  this = json_object_new_string ((gchar *) value);
  json_object_object_add (root, (gchar *) name, this);

  return FALSE;
}

static void
tf_json_append(GString *result, ValuePairs *vp, LogMessage *msg)
{
  struct json_object *json;

  json = json_object_new_object();
  json->_to_json_string = tf_json_object_to_string;

  value_pairs_walk(vp,
                   tf_json_obj_start, tf_json_value, tf_json_obj_end,
                   msg, 0, json);

  g_string_append(result, json_object_to_json_string (json));
  json_object_put(json);
}

static void
tf_json_call(LogTemplateFunction *self, gpointer s,
	     const LogTemplateInvokeArgs *args, GString *result)
{
  TFJsonState *state = (TFJsonState *)s;
  gint i;

  for (i = 0; i < args->num_messages; i++)
    tf_json_append(result, state->vp, args->messages[i]);
}

static void
tf_json_free_state(gpointer s)
{
  TFJsonState *state = (TFJsonState *)s;

  if (state->vp)
    value_pairs_free(state->vp);
  tf_simple_func_free_state(&state->super);
}

TEMPLATE_FUNCTION(TFJsonState, tf_json, tf_json_prepare, NULL, tf_json_call,
		  tf_json_free_state, NULL);

extern CfgParser jsonparser_parser;

static Plugin json_plugins[] =
  {
    {
      .type = LL_CONTEXT_PARSER,
      .name = "json-parser",
      .parser = &jsonparser_parser,
    },
    TEMPLATE_FUNCTION_PLUGIN(tf_json, "format_json"),
  };

gboolean
json_module_init(GlobalConfig *cfg, CfgArgs *args)
{
  plugin_register(cfg, json_plugins, G_N_ELEMENTS(json_plugins));
  return TRUE;
}

const ModuleInfo module_info =
{
  .canonical_name = "json",
  .version = VERSION,
  .description = "The json module provides JSON parsing & formatting support for syslog-ng.",
  .core_revision = SOURCE_REVISION,
  .plugins = json_plugins,
  .plugins_len = G_N_ELEMENTS(json_plugins),
};
