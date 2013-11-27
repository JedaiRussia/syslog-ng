/*
 * Copyright (c) 2002-2013 BalaBit IT Ltd, Budapest, Hungary
 * Copyright (c) 1998-2012 Balázs Scheidler
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
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
 * As an additional exemption you are allowed to compile & link against the
 * OpenSSL libraries as published by the OpenSSL project. See the file
 * COPYING for details.
 *
 */
  
#include "stats.h"
#include "stats-syslog.h"
#include "messages.h"
#include "timeutils.h"
#include "misc.h"

#include <string.h>
#include <iv.h>

/*
 * The statistics module
 *
 * Various components of syslog-ng require counters to keep track of various
 * metrics, such as number of messages processed, dropped or stored in a
 * queue. For this purpose, this module provides an easy to use API to
 * register and keep track these counters, and also to publish them to
 * external programs via a UNIX domain socket.
 *
 * Each counter has the following properties:
 *   * source component: enumerable type, that specifies the syslog-ng
 *     component that the given counter belongs to, examples:
 *       source.file, destination.file, center, source.socket, etc.
 *
 *   * id: the unique identifier of the syslog-ng configuration item that
 *     this counter belongs to. Named configuration elements (source,
 *     destination, etc) use their "name" here. Other components without a
 *     name use either an autogenerated ID (that can change when the
 *     configuration file changes), or an explicit ID configured by the
 *     administrator.
 * 
 *   * instance: each configuration element may track several sets of
 *     counters. This field specifies an identifier that makes a group of
 *     counters unique. For instance:
 *      - source TCP drivers use the IP address of the client here
 *      - destination file writers use the expanded filename
 *      - for those which have no notion for instance, NULL is used
 *
 *   * state: dynamic, active or orphaned, this indicates whether the given
 *     counter is in use or in orphaned state
 *
 *   * type: counter type (processed, dropped, stored, etc)
 *
 * Threading
 *
 * Once registered, changing the counters is thread safe (but see the
 * note on set/get), inc/dec is generally safe. To register counters,
 * the stats code must run in the main thread (assuming init/deinit is
 * running) or the stats lock must be acquired using stats_lock() and
 * stats_unlock(). This API is used to allow batching multiple stats
 * operations under the protection of the same lock acquiral.
 */

StatsOptions *stats_options;


static GHashTable *counter_hash;
static GStaticMutex stats_mutex = G_STATIC_MUTEX_INIT;
gboolean stats_locked;

static gboolean
stats_counter_equal(gconstpointer p1, gconstpointer p2)
{
  const StatsCluster *sc1 = (StatsCluster *) p1;
  const StatsCluster *sc2 = (StatsCluster *) p2;
  
  return sc1->source == sc2->source && strcmp(sc1->id, sc2->id) == 0 && strcmp(sc1->instance, sc2->instance) == 0;
}

static guint
stats_counter_hash(gconstpointer p)
{
  const StatsCluster *sc = (StatsCluster *) p;
  
  return g_str_hash(sc->id) + g_str_hash(sc->instance) + sc->source;
}

static void
stats_counter_free(gpointer p)
{ 
  StatsCluster *sc = (StatsCluster *) p;
  
  g_free(sc->id);
  g_free(sc->instance);
  g_free(sc);
}

gboolean
stats_check_level(gint level)
{
  if (stats_options)
    return (stats_options->level >= level);
  else
    return level == 0;
}

void
stats_lock(void)
{
  g_static_mutex_lock(&stats_mutex);
  stats_locked = TRUE;
}

void
stats_unlock(void)
{
  stats_locked = FALSE;
  g_static_mutex_unlock(&stats_mutex);
}

static StatsCluster *
stats_add_counter(gint stats_level, gint source, const gchar *id, const gchar *instance, gboolean *new)
{
  StatsCluster key;
  StatsCluster *sc;

  if (!stats_check_level(stats_level))
    return NULL;
  
  if (!id)
    id = "";
  if (!instance)
    instance = "";
  
  key.source = source;
  key.id = (gchar *) id;
  key.instance = (gchar *) instance;
  
  sc = g_hash_table_lookup(counter_hash, &key);
  if (!sc)
    {
      /* no such StatsCluster instance, register one */
      sc = g_new0(StatsCluster, 1);
      
      sc->source = source;
      sc->id = g_strdup(id);
      sc->instance = g_strdup(instance);
      sc->ref_cnt = 1;
      g_hash_table_insert(counter_hash, sc, sc);
      *new = TRUE;
    }
  else
    {
      if (sc->ref_cnt == 0)
        /* it just haven't been cleaned up */
        *new = TRUE;
      else
        *new = FALSE;

      sc->ref_cnt++;
    }

  return sc;
}

/**
 * stats_register_counter:
 * @stats_level: the required statistics level to make this counter available
 * @source: a reference to the syslog-ng component that this counter belongs to (SCS_*)
 * @id: the unique identifier of the configuration item that this counter belongs to
 * @instance: if a given configuration item manages multiple similar counters
 *            this makes those unique (like destination filename in case macros are used)
 * @type: the counter type (processed, dropped, etc)
 * @counter: returned pointer to the counter
 *
 * This fuction registers a general purpose counter. Whenever multiple
 * objects touch the same counter all of these should register the counter
 * with the same name. Internally the stats subsystem counts the number of
 * users of the same counter in this case, thus the counter will only be
 * freed when all of these uses are unregistered.
 **/
void
stats_register_counter(gint stats_level, gint source, const gchar *id, const gchar *instance, StatsCounterType type, StatsCounterItem **counter)
{
  StatsCluster *sc;
  gboolean new;

  g_assert(stats_locked);
  g_assert(type < SC_TYPE_MAX);
  
  *counter = NULL;
  sc = stats_add_counter(stats_level, source, id, instance, &new);
  if (!sc)
    return;

  *counter = &sc->counters[type];
  sc->live_mask |= 1 << type;
}

StatsCluster *
stats_register_dynamic_counter(gint stats_level, gint source, const gchar *id, const gchar *instance, StatsCounterType type, StatsCounterItem **counter, gboolean *new)
{
  StatsCluster *sc;
  gboolean local_new;

  g_assert(stats_locked);
  g_assert(type < SC_TYPE_MAX);
  
  *counter = NULL;
  *new = FALSE;
  sc = stats_add_counter(stats_level, source, id, instance, &local_new);
  if (new)
    *new = local_new;
  if (!sc)
    return NULL;

  if (!local_new && !sc->dynamic)
    g_assert_not_reached();

  sc->dynamic = TRUE;
  *counter = &sc->counters[type];
  sc->live_mask |= 1 << type;
  return sc;
}

/*
 * stats_instant_inc_dynamic_counter
 * @timestamp: if non-negative, an associated timestamp will be created and set
 *
 * Instantly create (if not exists) and increment a dynamic counter.
 */
void
stats_register_and_increment_dynamic_counter(gint stats_level, gint source_mask, const gchar *id, const gchar *instance, time_t timestamp)
{
  StatsCounterItem *counter, *stamp;
  gboolean new;
  StatsCluster *handle;

  g_assert(stats_locked);
  handle = stats_register_dynamic_counter(stats_level, source_mask, id, instance, SC_TYPE_PROCESSED, &counter, &new);
  stats_counter_inc(counter);
  if (timestamp >= 0)
    {
      stats_register_associated_counter(handle, SC_TYPE_STAMP, &stamp);
      stats_counter_set(stamp, timestamp);
      stats_unregister_dynamic_counter(handle, SC_TYPE_STAMP, &stamp);
    }
  stats_unregister_dynamic_counter(handle, SC_TYPE_PROCESSED, &counter);
}

/**
 * stats_register_associated_counter:
 * @sc: the dynamic counter that was registered with stats_register_dynamic_counter
 * @type: the type that we want to use in the same StatsCluster instance
 * @counter: the returned pointer to the counter itself
 *
 * This function registers another counter type in the same StatsCounter
 * instance in order to avoid an unnecessary lookup.
 **/
void
stats_register_associated_counter(StatsCluster *sc, StatsCounterType type, StatsCounterItem **counter)
{
  g_assert(stats_locked);

  *counter = NULL;
  if (!sc)
    return;
  g_assert(sc->dynamic);

  *counter = &sc->counters[type];
  sc->live_mask |= 1 << type;
  sc->ref_cnt++;
}

void
stats_unregister_counter(gint source, const gchar *id, const gchar *instance, StatsCounterType type, StatsCounterItem **counter)
{
  StatsCluster *sc;
  StatsCluster key;
  
  g_assert(stats_locked);

  if (*counter == NULL)
    return;

  if (!id)
    id = "";
  if (!instance)
    instance = "";
  
  key.source = source;
  key.id = (gchar *) id;
  key.instance = (gchar *) instance;

  sc = g_hash_table_lookup(counter_hash, &key);

  g_assert(sc && (sc->live_mask & (1 << type)) && &sc->counters[type] == (*counter));
  
  *counter = NULL;
  sc->ref_cnt--;
}

void
stats_unregister_dynamic_counter(StatsCluster *sc, StatsCounterType type, StatsCounterItem **counter)
{
  g_assert(stats_locked);
  if (!sc)
    return;
  g_assert(sc && (sc->live_mask & (1 << type)) && &sc->counters[type] == (*counter));
  sc->ref_cnt--;
}

static void
_foreach_cluster_helper(gpointer key, gpointer value, gpointer user_data)
{
  gpointer *args = (gpointer *) user_data;
  StatsForeachClusterFunc func = args[0];
  gpointer func_data = args[1];
  StatsCluster *sc = (StatsCluster *) value;
  
  func(sc, func_data);
}

void
stats_foreach_cluster(StatsForeachClusterFunc func, gpointer user_data)
{
  gpointer args[] = { func, user_data };

  g_assert(stats_locked);
  g_hash_table_foreach(counter_hash, _foreach_cluster_helper, args);
}

static gboolean
_foreach_cluster_remove_helper(gpointer key, gpointer value, gpointer user_data)
{
  gpointer *args = (gpointer *) user_data;
  StatsForeachClusterRemoveFunc func = args[0];
  gpointer func_data = args[1];
  StatsCluster *sc = (StatsCluster *) value;
  
  return func(sc, func_data);
}

void
stats_foreach_cluster_remove(StatsForeachClusterRemoveFunc func, gpointer user_data)
{
  gpointer args[] = { func, user_data };
  g_hash_table_foreach_remove(counter_hash, _foreach_cluster_remove_helper, args);
}

static void
_foreach_counter_helper(StatsCluster *sc, gpointer user_data)
{
  gpointer *args = (gpointer *) user_data;
  StatsForeachCounterFunc func = args[0];
  gpointer func_data = args[1];
  
  stats_cluster_foreach_counter(sc, func, func_data);
}

void
stats_foreach_counter(StatsForeachCounterFunc func, gpointer user_data)
{
  gpointer args[] = { func, user_data };

  g_assert(stats_locked);
  stats_foreach_cluster(_foreach_counter_helper, args);
}

const gchar *tag_names[SC_TYPE_MAX] =
{
  /* [SC_TYPE_DROPPED]   = */ "dropped",
  /* [SC_TYPE_PROCESSED] = */ "processed",
  /* [SC_TYPE_STORED]   = */  "stored",
  /* [SC_TYPE_SUPPRESSED] = */ "suppressed",
  /* [SC_TYPE_STAMP] = */ "stamp",
};

const gchar *source_names[SCS_MAX] =
{
  "none",
  "file",
  "pipe",
  "tcp",
  "udp",
  "tcp6",
  "udp6",
  "unix-stream",
  "unix-dgram",
  "syslog",
  "network",
  "internal",
  "logstore",
  "program",
  "sql",
  "sun-streams",
  "usertty",
  "group",
  "center",
  "host",
  "global",
  "mongodb",
  "class",
  "rule_id",
  "tag",
  "severity",
  "facility",
  "sender",
  "smtp",
  "amqp",
  "stomp",
  "redis",
  "snmp",
};

const gchar *
stats_get_direction_name(gint source)
{
  return (source & SCS_SOURCE ? "src." : (source & SCS_DESTINATION ? "dst." : ""));
}

const gchar *
stats_get_source_name(gint source)
{
  return source_names[source & SCS_SOURCE_MASK];
}

const gchar *
stats_get_tag_name(gint type)
{
  return tag_names[type];
}

/* buf is a scratch area which is not always used, the return value is
 * either a locally managed string or points to @buf.  */
const gchar *
stats_get_direction_and_source_name(gint source, gchar *buf, gsize buf_len)
{
  if ((source & SCS_SOURCE_MASK) == SCS_GROUP)
    {
      if (source & SCS_SOURCE)
        return "source";
      else if (source & SCS_DESTINATION)
        return "destination";
      else
        g_assert_not_reached();
    }
  else
    {
      g_snprintf(buf, buf_len, "%s%s", stats_get_direction_name(source), stats_get_source_name(source));
      return buf;
    }
}

static void
stats_log_format_counter(StatsCluster *sc, gint type, StatsCounterItem *item, gpointer user_data)
{
  EVTREC *e = (EVTREC *) user_data;
  EVTTAG *tag;
  gchar buf[32];

  tag = evt_tag_printf(stats_get_tag_name(type), "%s(%s%s%s)=%u", 
                       stats_get_direction_and_source_name(sc->source, buf, sizeof(buf)),
                       sc->id,
                       (sc->id[0] && sc->instance[0]) ? "," : "",
                       sc->instance,
                       stats_counter_get(&sc->counters[type]));
  evt_rec_add_tag(e, tag);
}


static void
stats_log_format_cluster(StatsCluster *sc, EVTREC *e)
{
  stats_cluster_foreach_counter(sc, stats_log_format_counter, e);
}

static gboolean
stats_counter_is_expired(StatsCluster *sc, time_t now)
{
  time_t tstamp;

  /* check if dynamic entry, non-dynamic entries cannot be too large in
   * numbers, those are never pruned */
  if (!sc->dynamic)
    return FALSE;

  /* this entry is being updated, cannot be too old */    
  if (sc->ref_cnt > 0)
    return FALSE;

  /* check if timestamp is stored, no timestamp means we can't expire it.
   * All dynamic entries should have a timestamp.  */
  if ((sc->live_mask & (1 << SC_TYPE_STAMP)) == 0)
    return FALSE;

  tstamp = sc->counters[SC_TYPE_STAMP].value;
  return (tstamp <= now - stats_options->lifetime);
}

typedef struct _StatsTimerState
{
  GTimeVal now;
  time_t oldest_counter;
  gint dropped_counters;
  EVTREC *stats_event;
} StatsTimerState;

static gboolean
stats_prune_counter(StatsCluster *sc, StatsTimerState *st)
{
  gboolean expired;

  expired = stats_counter_is_expired(sc, st->now.tv_sec);
  if (expired)
    {
      time_t tstamp = sc->counters[SC_TYPE_STAMP].value;
      if ((st->oldest_counter) == 0 || st->oldest_counter > tstamp)
        st->oldest_counter = tstamp;
      st->dropped_counters++;
    }
  return expired;
}

static gboolean
stats_format_and_prune_cluster(StatsCluster *sc, gpointer user_data)
{
  StatsTimerState *st = (StatsTimerState *) user_data;

  stats_log_format_cluster(sc, st->stats_event);
  return stats_prune_counter(sc, st);
}

void
stats_publish_and_prune_counters(void)
{
  StatsTimerState st;
  gboolean publish = (stats_options->log_freq > 0);
  
  st.oldest_counter = 0;
  st.dropped_counters = 0;
  st.stats_event = NULL;
  cached_g_current_time(&st.now);

  if (publish)
    st.stats_event = msg_event_create(EVT_PRI_INFO, "Log statistics", NULL);

  stats_lock();
  stats_foreach_cluster_remove(stats_format_and_prune_cluster, &st);
  stats_unlock();

  if (publish)
    msg_event_send(st.stats_event);

  if (st.dropped_counters > 0)
    {
      msg_notice("Pruning stats-counters have finished",
                 evt_tag_int("dropped", st.dropped_counters),
                 evt_tag_long("oldest-timestamp", (long) st.oldest_counter),
                 NULL);
    }
}


static void
stats_timer_rearm(struct iv_timer *timer)
{
  gint freq = GPOINTER_TO_INT(timer->cookie);
  if (freq > 0)
    {
      /* arm the timer */
      iv_validate_now();
      timer->expires = iv_now;
      timespec_add_msec(&timer->expires, freq * 1000);
      iv_timer_register(timer);
    }
}

static void
stats_timer_init(struct iv_timer *timer, void (*handler)(void *), gint freq)
{
  IV_TIMER_INIT(timer);
  timer->handler = handler;
  timer->cookie = GINT_TO_POINTER(freq);
}

static void
stats_timer_kill(struct iv_timer *timer)
{
  if (!timer->handler)
    return;
  if (iv_timer_registered(timer))
    iv_timer_unregister(timer);
}

static struct iv_timer stats_timer;


static void
stats_timer_elapsed(gpointer st)
{
  stats_publish_and_prune_counters();
  stats_timer_rearm(&stats_timer);
}

void
stats_timer_reinit(void)
{
  gint freq;

  freq = stats_options->log_freq;
  if (!freq)
    freq = stats_options->lifetime <= 1 ? 1 : stats_options->lifetime / 2;

  stats_timer_kill(&stats_timer);
  stats_timer_init(&stats_timer, stats_timer_elapsed, freq);
  stats_timer_rearm(&stats_timer);
}

void
stats_reinit(StatsOptions *options)
{
  stats_options = options;
  stats_syslog_reinit();
  stats_timer_reinit();
}

void
stats_init(void)
{
  counter_hash = g_hash_table_new_full(stats_counter_hash, stats_counter_equal, NULL, stats_counter_free);
  g_static_mutex_init(&stats_mutex);
}

void
stats_destroy(void)
{
  g_hash_table_destroy(counter_hash);
  counter_hash = NULL;
  g_static_mutex_free(&stats_mutex);
}

void
stats_options_defaults(StatsOptions *options)
{
  options->level = 0;
  options->log_freq = 600;
  options->lifetime = 600;
}
