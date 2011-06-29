/* Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements. 
 * The ASF licenses this file to You under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License.  You may obtain a copy of the License at
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
 * Implementation of the HTTP Test Tool module.
 */

/************************************************************************
 * Includes
 ***********************************************************************/
#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <openssl/ssl.h>

#include <apr.h>
#include <apr_lib.h>
#include <apr_errno.h>
#include <apr_strings.h>
#include <apr_network_io.h>
#include <apr_thread_proc.h>
#include <apr_thread_cond.h>
#include <apr_thread_mutex.h>
#include <apr_portable.h>
#include <apr_hash.h>
#include <apr_base64.h>

#include <pcre.h>
#if APR_HAVE_UNISTD_H
#include <unistd.h> /* for getpid() */
#endif

#include "defines.h"
#include "util.h"
#include "regex.h"
#include "file.h"
#include "socket.h"
#include "ssl.h"
#include "worker.h"
#include "module.h"


/************************************************************************
 * Definitions 
 ***********************************************************************/

/************************************************************************
 * Globals 
 ***********************************************************************/

/************************************************************************
 * Implementation
 ***********************************************************************/
apr_status_t module_command_new(global_t *global, const char *module, 
                                const char *command,
				const char *short_desc, const char *desc, 
				interpret_f function) {
  apr_status_t status;
  worker_t *worker;
  apr_hash_t *blocks;

  if ((status = worker_new(&worker, "", "", global, function)) != APR_SUCCESS) {
    return status;
  }

  /* descriptions */
  worker->short_desc = short_desc;
  worker->desc = desc;

  if (!(blocks = apr_hash_get(global->modules, module, APR_HASH_KEY_STRING))) {
    blocks = apr_hash_make(global->pool);
    apr_hash_set(global->modules, module, APR_HASH_KEY_STRING, blocks); 
  }

  /* add workers to commands hash */
  apr_hash_set(blocks, command, APR_HASH_KEY_STRING, worker);
  return APR_SUCCESS;
}

