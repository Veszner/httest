/**
 * Copyright 2010 Christian Liesch
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/**
 * @file
 *
 * @Author christian liesch <liesch@gmx.ch>
 *
 * Implementation of the HTTP Test Tool skeleton module 
 */

/************************************************************************
 * Includes
 ***********************************************************************/
#include "module.h"
#include "tcp_module.h"

/************************************************************************
 * Definitions 
 ***********************************************************************/
typedef struct perf_time_s {
  apr_time_t cur;
  apr_time_t min;
  apr_time_t avr;
  apr_time_t max;
  apr_time_t total;
} perf_time_t;

typedef struct perf_count_s {
  int reqs;
  int conns;
  int less[10];
  int status[600];
} perf_count_t;

typedef struct perf_s {
  perf_count_t count;
  apr_size_t recv_bytes;
  apr_size_t sent_bytes;
  perf_time_t conn_time;
  perf_time_t recv_time;
  perf_time_t sent_time;
  apr_time_t sent_time_total;
} perf_t;

typedef struct perf_wconf_s {
  apr_time_t start_time;
  int cur_status;
  const char *request_line;
  perf_t stat;
} perf_wconf_t;

typedef struct perf_host_s {
  char *name;
  int clients;
  int state;
#define PERF_HOST_NONE      0
#define PERF_HOST_CONNECTED 1
#define PERF_HOST_ERROR 2
  apr_thread_mutex_t *sync_mutex;
} perf_host_t;

typedef struct perf_gconf_s {
  int on;
#define PERF_GCONF_OFF  0
#define PERF_GCONF_ON   1
#define PERF_GCONF_LOG  2 
  int flags;
#define PERF_GCONF_FLAGS_NONE 0 
#define PERF_GCONF_FLAGS_DIST 1 
  apr_file_t *log_file;
  apr_hash_t *host_and_ports;
  apr_hash_index_t *cur_host_i;
  perf_host_t *cur_host;
  perf_t stat;
} perf_gconf_t;

/************************************************************************
 * Globals 
 ***********************************************************************/
const char * perf_module = "perf_module";

/************************************************************************
 * Local 
 ***********************************************************************/

/**
 * Get stat config from global 
 *
 * @param global IN 
 * @return stat config
 */
static perf_gconf_t *perf_get_global_config(global_t *global) {
  perf_gconf_t *config = module_get_config(global->config, perf_module);
  if (config == NULL) {
    config = apr_pcalloc(global->pool, sizeof(*config));
    config->host_and_ports = apr_hash_make(global->pool);
    module_set_config(global->config, apr_pstrdup(global->pool, perf_module), config);
  }
  return config;
}

/**
 * Get stat config from worker
 *
 * @param worker IN worker
 * @return stat config
 */
static perf_wconf_t *perf_get_worker_config(worker_t *worker) {
  perf_wconf_t *config = module_get_config(worker->config, perf_module);
  if (config == NULL) {
    config = apr_pcalloc(worker->pbody, sizeof(*config));
    module_set_config(worker->config, apr_pstrdup(worker->pbody, perf_module), config);
  }
  return config;
}

/**
 * Test if statistic is turned on 
 * @param global IN global config
 * @param line IN read line
 */
static apr_status_t perf_read_line(global_t *global, char **line) {

  if (strncmp(*line, "PERF:STAT", 9) == 0) {
    perf_gconf_t *gconf = perf_get_global_config(global);
    char *cur;
    char *last;

    apr_strtok(*line, " ", &last);
    cur = apr_strtok(NULL, " ", &last);
    if (strcmp(cur, "ON") == 0) {
      gconf->on = PERF_GCONF_ON;
    }
    else if (strcmp(cur, "OFF") == 0) {
      gconf->on |= PERF_GCONF_OFF;
    }
    else if (strcmp(cur, "LOG") == 0) {
      apr_status_t status;
      char *filename;
      gconf->on |= PERF_GCONF_LOG;
      filename = apr_strtok(NULL, " ", &last);
      if ((status = apr_file_open(&gconf->log_file, filename, 
                                  APR_READ|APR_WRITE|APR_CREATE|APR_APPEND|APR_XTHREAD, 
                                  APR_OS_DEFAULT, global->pool)) != APR_SUCCESS) {
        fprintf(stderr, "Could not open log file \"%s\"", filename);
        return status;
      }
    }
  }
  else if (strncmp(*line, "PERF:DISTRIBUTE", 9) == 0) {
    char *last;
    char *name;
    perf_host_t *host = apr_pcalloc(global->pool, sizeof(*host));
    perf_gconf_t *gconf = perf_get_global_config(global);
    gconf->flags |= PERF_GCONF_FLAGS_DIST;
    apr_strtok(*line, " ", &last);
    name = apr_strtok(NULL, " ", &last);
    while (name) {
      host->name = name;
      apr_hash_set(gconf->host_and_ports, name, APR_HASH_KEY_STRING, host);
      perf_host_t *host = apr_pcalloc(global->pool, sizeof(*host));
      name = apr_strtok(NULL, " ", &last);
    }
  }
  return APR_SUCCESS;
}

/**
 * Is called after line is sent
 * @param worker IN callee
 * @param line IN line sent
 * @return APR_SUCCESS
 */
static apr_status_t perf_line_sent(worker_t *worker, line_t *line) {
  global_t *global = worker->global;
  perf_wconf_t *wconf = perf_get_worker_config(worker);
  perf_gconf_t *gconf = perf_get_global_config(global);

  if (gconf->on & PERF_GCONF_ON && worker->flags & FLAGS_CLIENT) {
    if (wconf->start_time == 0) {
      ++wconf->stat.count.reqs;
      wconf->start_time = apr_time_now();
      wconf->request_line = line->buf;
    }
    wconf->stat.sent_bytes += line->len;
    if (strncmp(line->info, "NOCRLF", 6) != 0) {
      wconf->stat.sent_bytes += 2;
    }
  }
  return APR_SUCCESS;
}

/**
 * Is before request receive
 * @param worker IN callee
 * @param line IN line sent
 * @return APR_SUCCESS
 */
static apr_status_t perf_WAIT_begin(worker_t *worker) {
  global_t *global = worker->global;
  perf_wconf_t *wconf = perf_get_worker_config(worker);
  perf_gconf_t *gconf = perf_get_global_config(global);

  if (gconf->on & PERF_GCONF_ON && worker->flags & FLAGS_CLIENT) {
    apr_time_t now = apr_time_now();
    apr_time_t duration = now - wconf->start_time;
    wconf->start_time = now;
    wconf->stat.sent_time.cur = duration;
    wconf->stat.sent_time_total += duration;
    if (duration > wconf->stat.sent_time.max) {
      wconf->stat.sent_time.max = duration;
    }
    if (duration < wconf->stat.sent_time.min || wconf->stat.sent_time.min == 0) {
      wconf->stat.sent_time.min = duration;
    }
  }
  return APR_SUCCESS;
}

/**
 * Get status line length and count 200, 302, 400 and 500 errors 
 * @param worker IN callee
 * @param line IN received status line
 * @return APR_SUCCESS
 */
static apr_status_t perf_read_status_line(worker_t *worker, char *line) {
  global_t *global = worker->global;
  perf_wconf_t *wconf = perf_get_worker_config(worker);
  perf_gconf_t *gconf = perf_get_global_config(global);

  if (gconf->on & PERF_GCONF_ON && worker->flags & FLAGS_CLIENT) {
    char *cur;
    wconf->stat.recv_bytes += strlen(line) + 2;
    if ((cur = strstr(line, " "))) {
      int status;
      ++cur;
      status = apr_atoi64(cur);
      ++wconf->stat.count.status[status];
      wconf->cur_status = status;
    }

  }   
  return APR_SUCCESS;
}

/**
 * Get line length
 * @param worker IN callee
 * @param line IN received status line
 * @return APR_SUCCESS
 */
static apr_status_t perf_read_header(worker_t *worker, char *line) {
  global_t *global = worker->global;
  perf_wconf_t *wconf = perf_get_worker_config(worker);
  perf_gconf_t *gconf = perf_get_global_config(global);

  if (gconf->on & PERF_GCONF_ON && worker->flags & FLAGS_CLIENT) {
    wconf->stat.recv_bytes += strlen(line) + 2;
  }   
  return APR_SUCCESS;
}

/**
 * Get buf length
 * @param worker IN callee
 * @param line IN received status line
 * @return APR_SUCCESS
 */
static apr_status_t perf_read_buf(worker_t *worker, char *buf, apr_size_t len) {
  global_t *global = worker->global;
  perf_wconf_t *wconf = perf_get_worker_config(worker);
  perf_gconf_t *gconf = perf_get_global_config(global);

  if (gconf->on & PERF_GCONF_ON && worker->flags & FLAGS_CLIENT) {
    wconf->stat.recv_bytes += len + 2;
  }   
  return APR_SUCCESS;
}

/**
 * Measure response time
 * @param worker IN callee
 * @param status IN apr status
 * @return received status 
 */
static apr_status_t perf_WAIT_end(worker_t *worker, apr_status_t status) {
  global_t *global = worker->global;
  perf_wconf_t *wconf = perf_get_worker_config(worker);
  perf_gconf_t *gconf = perf_get_global_config(global);

  if (gconf->on & PERF_GCONF_ON && worker->flags & FLAGS_CLIENT) {
    int i;
    apr_time_t compare;
    apr_time_t now = apr_time_now();
    apr_time_t duration = now - wconf->start_time;
    wconf->start_time = 0;
    wconf->stat.recv_time.cur = duration;
    wconf->stat.recv_time.total += duration;
    if (duration > wconf->stat.recv_time.max) {
      wconf->stat.recv_time.max = duration;
    }
    if (duration < wconf->stat.recv_time.min || wconf->stat.recv_time.min == 0) {
      wconf->stat.recv_time.min = duration;
    }
    for (i = 0, compare = 1; i < 10; i++, compare *= 2) {
      apr_time_t t = apr_time_sec(wconf->stat.sent_time.cur + wconf->stat.recv_time.cur);
      if (t < compare) {
        ++wconf->stat.count.less[i];
        break;
      }
    }
  }
  if (gconf->on & PERF_GCONF_LOG && worker->flags & FLAGS_CLIENT) {
    apr_pool_t *pool;
    char *date_str;

    apr_pool_create(&pool, NULL);
    date_str = apr_palloc(pool, APR_RFC822_DATE_LEN);
    apr_rfc822_date(date_str, apr_time_now());
    apr_file_printf(gconf->log_file, "[%s] \"%s\" %d %"APR_TIME_T_FMT" %"APR_TIME_T_FMT"\n", 
                    date_str,  wconf->request_line, wconf->cur_status, 
                    wconf->stat.sent_time.cur, wconf->stat.recv_time.cur);
    apr_pool_destroy(pool);
  }
  return status;
}

/**
 * Start connect timer
 * @param worker IN callee
 * @param line IN received status line
 * @return APR_SUCCESS
 */
static apr_status_t perf_pre_connect(worker_t *worker) {
  global_t *global = worker->global;
  perf_wconf_t *wconf = perf_get_worker_config(worker);
  perf_gconf_t *gconf = perf_get_global_config(global);

  if (gconf->on & PERF_GCONF_ON && worker->flags & FLAGS_CLIENT) {
    wconf->stat.conn_time.cur = apr_time_now();
    ++wconf->stat.count.conns;
  }
  return APR_SUCCESS;
}

/**
 * Stop connect timer and measure connection time
 * @param worker IN callee
 * @param line IN received status line
 * @return APR_SUCCESS
 */
static apr_status_t perf_post_connect(worker_t *worker) {
  global_t *global = worker->global;
  perf_wconf_t *wconf = perf_get_worker_config(worker);
  perf_gconf_t *gconf = perf_get_global_config(global);

  if (gconf->on & PERF_GCONF_ON && worker->flags & FLAGS_CLIENT) {
    apr_time_t duration = apr_time_now() - wconf->stat.conn_time.cur;
    wconf->stat.conn_time.cur = duration;
    wconf->stat.conn_time.total += duration;
    if (duration > wconf->stat.conn_time.max) {
      wconf->stat.conn_time.max = duration;
    }
    if (duration < wconf->stat.conn_time.min || wconf->stat.conn_time.min == 0) {
      wconf->stat.conn_time.min = duration;
    }

  }
  return APR_SUCCESS;
}

/**
 * Collect all data and store it in global
 * @param worker IN callee
 * @param line IN received status line
 * @return APR_SUCCESS
 */
static apr_status_t perf_worker_finally(worker_t *worker) {
  perf_wconf_t *wconf = perf_get_worker_config(worker);
  perf_gconf_t *gconf = perf_get_global_config(worker->global);
  if (gconf->on & PERF_GCONF_ON && worker->flags & FLAGS_CLIENT) {
    int i;
    apr_thread_mutex_lock(worker->mutex);
    if (wconf->stat.sent_time.max > gconf->stat.sent_time.max) {
      gconf->stat.sent_time.max = wconf->stat.sent_time.max;
    }
    if (wconf->stat.recv_time.max > gconf->stat.recv_time.max) {
      gconf->stat.recv_time.max = wconf->stat.recv_time.max;
    }
    if (wconf->stat.conn_time.max > gconf->stat.conn_time.max) {
      gconf->stat.conn_time.max = wconf->stat.conn_time.max;
    }
    if (wconf->stat.sent_time.min < gconf->stat.sent_time.min || gconf->stat.sent_time.min == 0) {
      gconf->stat.sent_time.min = wconf->stat.sent_time.min;
    }
    if (wconf->stat.recv_time.min < gconf->stat.recv_time.min || gconf->stat.recv_time.min == 0) {
      gconf->stat.recv_time.min = wconf->stat.recv_time.min;
    }
    if (wconf->stat.conn_time.min < gconf->stat.conn_time.min || gconf->stat.conn_time.min == 0) {
      gconf->stat.conn_time.min = wconf->stat.conn_time.min;
    }
    gconf->stat.sent_bytes += wconf->stat.sent_bytes;
    gconf->stat.recv_bytes += wconf->stat.recv_bytes;
    gconf->stat.sent_time_total += wconf->stat.sent_time_total;
    gconf->stat.recv_time.total += wconf->stat.recv_time.total;
    gconf->stat.conn_time.total += wconf->stat.conn_time.total;
    gconf->stat.count.reqs += wconf->stat.count.reqs;
    gconf->stat.count.conns += wconf->stat.count.conns;
    for (i = 0; i < 10; i++) {
      gconf->stat.count.less[i] += wconf->stat.count.less[i];
    }
    for (i = 0; i < 600; i++) {
      gconf->stat.count.status[i] += wconf->stat.count.status[i];
    }
    apr_thread_mutex_unlock(worker->mutex);
  }
  return APR_SUCCESS;
}

/**
 * Display collected data
 * @param worker IN callee
 * @param line IN received status line
 * @return APR_SUCCESS
 */
static apr_status_t perf_worker_joined(global_t *global) {
  perf_gconf_t *gconf = perf_get_global_config(global);
  if (gconf->on & PERF_GCONF_ON) {
    int i; 
    apr_time_t time;
    gconf->stat.sent_time.avr = gconf->stat.sent_time_total/gconf->stat.count.reqs;
    gconf->stat.recv_time.avr = gconf->stat.recv_time.total/gconf->stat.count.reqs;
    gconf->stat.conn_time.avr = gconf->stat.conn_time.total/gconf->stat.count.conns;
    fprintf(stdout, "\ntotal reqs: %d\n", gconf->stat.count.reqs);
    fprintf(stdout, "total conns: %d\n", gconf->stat.count.conns);
    fprintf(stdout, "send bytes: %d\n", gconf->stat.sent_bytes);
    fprintf(stdout, "received bytes: %d\n", gconf->stat.recv_bytes);
    for (i = 0, time = 1; i < 10; i++, time *= 2) {
      if (gconf->stat.count.less[i]) {
        fprintf(stdout, "%d request%s less than %"APR_TIME_T_FMT" seconds\n", 
                gconf->stat.count.less[i], gconf->stat.count.less[i]>1?"s":"", time);
      }
    }
    for (i = 0; i < 600; i++) {
      if (gconf->stat.count.status[i]) {
        fprintf(stdout, "status %d: %d\n", i, gconf->stat.count.status[i]);
      }
    }
    fprintf(stdout, "\nconn min: %"APR_TIME_T_FMT" max: %"APR_TIME_T_FMT " avr: %"APR_TIME_T_FMT "\n", 
            gconf->stat.conn_time.min, gconf->stat.conn_time.max, gconf->stat.conn_time.avr);
    fprintf(stdout, "sent min: %"APR_TIME_T_FMT" max: %"APR_TIME_T_FMT " avr: %"APR_TIME_T_FMT "\n", 
            gconf->stat.sent_time.min, gconf->stat.sent_time.max, gconf->stat.sent_time.avr);
    fprintf(stdout, "recv min: %"APR_TIME_T_FMT" max: %"APR_TIME_T_FMT " avr: %"APR_TIME_T_FMT "\n", 
            gconf->stat.recv_time.min, gconf->stat.recv_time.max, gconf->stat.recv_time.avr);
    fflush(stdout);
  }
  if (gconf->on & PERF_GCONF_LOG) {
    apr_file_close(gconf->log_file);
  }
  return APR_SUCCESS;
}

/**
 * Get cur host from hash
 * @param gconf IN global config
 * @return cur host
 */
static perf_host_t *perf_get_cur_host(perf_gconf_t *gconf) {
  void *val = NULL;
  if (gconf->cur_host_i) {
    apr_hash_this(gconf->cur_host_i, NULL, NULL, &val);
  }
  gconf->cur_host = val;
  return val;
}

/**
 * Get first remote host from hash
 * @param global IN global instance
 */
static perf_host_t *perf_get_first_host(global_t *global) {
  perf_gconf_t *gconf = perf_get_global_config(global);
  gconf->cur_host_i = apr_hash_first(global->pool, gconf->host_and_ports);
  return perf_get_cur_host(gconf);
}

/**
 * Get next remote host from hash
 * @param global IN global instance
 */
static perf_host_t *perf_get_next_host(global_t *global) {
  perf_gconf_t *gconf = perf_get_global_config(global);
  gconf->cur_host_i = apr_hash_next(gconf->cur_host_i);
  return perf_get_cur_host(gconf);
}

/**
 * Serialize to httestd
 * @param worker IN callee
 * @param fmt IN format
 * @param ... IN
 * @return apr_status_t
 */
static apr_status_t perf_serialize(worker_t *worker, char *fmt, ...) {
  char *tmp;
  va_list va;
  apr_pool_t *pool;

  apr_pool_create(&pool, NULL);
  va_start(va, fmt);
  tmp = apr_pvsprintf(pool, fmt, va);
  transport_write(worker->socket->transport, tmp, strlen(tmp));
  va_end(va);
  apr_pool_destroy(pool);

  return APR_SUCCESS;
}

/**
 * Iterate all modules and blocks for serialization
 * @param worker IN callee
 */
static apr_status_t perf_serialize_globals(worker_t *worker) {
  int i;
  apr_table_t *vars;
  apr_table_t *shared;
  apr_table_entry_t *e;
  apr_pool_t *ptmp;
  global_t *global = worker->global;

  apr_pool_create(&ptmp, NULL);
  vars = store_get_table(global->vars, ptmp);
  e = (apr_table_entry_t *) apr_table_elts(vars)->elts;
  for (i = 0; i < apr_table_elts(vars)->nelts; ++i) {
    perf_serialize(worker, "SET %s=%s\n", e[i].key, e[i].val);
  }
  shared = store_get_table(global->vars, ptmp);
  e = (apr_table_entry_t *) apr_table_elts(shared)->elts;
  for (i = 0; i < apr_table_elts(shared)->nelts; ++i) {
    perf_serialize(worker, "GLOBAL %s=%s\n", e[i].key, e[i].val);
  }
  apr_pool_destroy(ptmp);
  return APR_SUCCESS;
}

static void * APR_THREAD_FUNC perf_thread_super(apr_thread_t * thread, void *selfv) {
  return NULL;
}

/**
 * Distribute host to remote host, start a supervisor thread
 * @worker IN callee
 * @return supervisor thread handle
 */
static apr_status_t perf_distribute_host(worker_t *worker, apr_thread_t **handle) {
  apr_status_t status;
  global_t *global = worker->global;
  perf_gconf_t *gconf = perf_get_global_config(global);
  apr_pool_t *ptmp;

  *handle = NULL;
  apr_pool_create(&ptmp, NULL);
  if (!(gconf->cur_host->state == PERF_HOST_CONNECTED) &&
      !(gconf->cur_host->state == PERF_HOST_ERROR)) {
    char *portname;
    char *hostport = apr_pstrdup(ptmp, gconf->cur_host->name);
    char *hostname = apr_strtok(hostport, ":", &portname);
    
    worker_get_socket(worker, hostname, portname);

    if ((status = tcp_connect(worker, hostname, portname)) != APR_SUCCESS) {
      gconf->cur_host->state = PERF_HOST_ERROR;
      worker_log_error(worker, "Could not connect to httestd \"%s\" SKIP", gconf->cur_host->name);
      apr_pool_destroy(ptmp);
      return status;
    }
    htt_run_connect(worker);
    gconf->cur_host->state = PERF_HOST_CONNECTED;
    perf_serialize_globals(worker);
    ++gconf->cur_host->clients;
    if ((status = apr_thread_mutex_create(&gconf->cur_host->sync_mutex,
                                          APR_THREAD_MUTEX_DEFAULT, global->pool))
        != APR_SUCCESS) {
      worker_log_error(worker, "Could not create super visor sync mutex");
      return status;
    }
    apr_thread_mutex_lock(gconf->cur_host->sync_mutex);
    if ((status = apr_thread_create(handle, global->tattr, perf_thread_super,
                                    worker, global->pool)) != APR_SUCCESS) {
      worker_log_error(worker, "Could not create supervisor thread");
      return status;
    }
  }
  else if ((gconf->cur_host->state == PERF_HOST_ERROR)) {
    worker_log_error(worker, "Could not connect to httestd \"%s\" SKIP", gconf->cur_host->name);
    apr_pool_destroy(ptmp);
    return APR_ECONNREFUSED;
  }
  else {
    ++gconf->cur_host->clients;
  }

  apr_pool_destroy(ptmp);
  return APR_SUCCESS;
}

/**
 * Distribute client worker.
 * @param worker IN callee
 * @param func IN concurrent function to call
 * @param new_thread OUT thread handle of concurrent function
 * @return APR_ENOTHREAD if there is no schedul policy, else any apr status.
 */
static apr_status_t perf_client_create(worker_t *worker, apr_thread_start_t func, apr_thread_t **new_thread) {
  global_t *global = worker->global;
  perf_gconf_t *gconf = perf_get_global_config(global);
  
  if (gconf->flags & PERF_GCONF_FLAGS_DIST) {
    if (!gconf->cur_host_i) {
      worker_log(worker, LOG_INFO, "Distribute CLIENT to my self");
      perf_get_first_host(global);
      return APR_ENOTHREAD;
    }
    else {
      apr_status_t status;
      /* distribute to remote host */
      worker_log(worker, LOG_INFO, "Distribute CLIENT to %s", gconf->cur_host->name);
      status = perf_distribute_host(worker, new_thread);
      perf_get_next_host(global);
      if (status == APR_SUCCESS) {
        /* return APR_SUCCESS; */
        return APR_ENOTHREAD;
      }
      else {
        return APR_ENOTHREAD;
      }
    }
  }

  return APR_ENOTHREAD;
}

/**
 * Wait for distributed client worker.
 * @param worker IN callee
 * @param thread IN thread handle of concurrent function
 * @return APR_ENOTHREAD if there is no schedul policy, else any apr status.
 */
static apr_status_t perf_client_start(worker_t *worker, apr_thread_t *thread) {
  return APR_ENOTHREAD;
}

/************************************************************************
 * Commands 
 ***********************************************************************/

/************************************************************************
 * Module
 ***********************************************************************/
apr_status_t perf_module_init(global_t *global) {
  htt_hook_read_line(perf_read_line, NULL, NULL, 0);
  htt_hook_client_create(perf_client_create, NULL, NULL, 0);
  htt_hook_client_start(perf_client_start, NULL, NULL, 0);
  htt_hook_worker_joined(perf_worker_joined, NULL, NULL, 0);
  htt_hook_worker_finally(perf_worker_finally, NULL, NULL, 0);
  htt_hook_pre_connect(perf_pre_connect, NULL, NULL, 0);
  htt_hook_post_connect(perf_post_connect, NULL, NULL, 0);
  htt_hook_line_sent(perf_line_sent, NULL, NULL, 0);
  htt_hook_WAIT_begin(perf_WAIT_begin, NULL, NULL, 0);
  htt_hook_read_status_line(perf_read_status_line, NULL, NULL, 0);
  htt_hook_read_header(perf_read_header, NULL, NULL, 0);
  htt_hook_read_buf(perf_read_buf, NULL, NULL, 0);
  htt_hook_WAIT_end(perf_WAIT_end, NULL, NULL, 0);
  return APR_SUCCESS;
}

