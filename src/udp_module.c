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
 * Implementation of the HTTP Test Tool udp module 
 */

/************************************************************************
 * Includes
 ***********************************************************************/
#include "module.h"

/************************************************************************
 * Definitions 
 ***********************************************************************/
typedef struct udp_socket_config_s {
  apr_sockaddr_t *sendto;
  apr_sockaddr_t *recvfrom;
} udp_socket_config_t;

/************************************************************************
 * Globals 
 ***********************************************************************/
const char * udp_module = "udp_module";

/************************************************************************
 * Local 
 ***********************************************************************/
/**
 * GET ssl socket config from socket
 *
 * @param worker IN worker
 * @return socket config
 */
static udp_socket_config_t *udp_get_socket_config(worker_t *worker) {
  if (!worker || !worker->socket) {
    return NULL;
  }

  udp_socket_config_t *config = module_get_config(worker->socket->config, udp_module);
  if (config == NULL) {
    config = apr_pcalloc(worker->pbody, sizeof(*config));
    module_set_config(worker->socket->config, apr_pstrdup(worker->pbody, udp_module), config);
  }
  return config;
}

/**
 * Get os socket descriptor
 *
 * @param data IN void pointer to socket
 * @param desc OUT os socket descriptor
 * @return apr status
 */
apr_status_t udp_transport_os_desc_get(void *data, int *desc) {
  worker_t *worker = data;
  return apr_os_sock_get(desc, worker->socket->socket);
}

/**
 * Set timeout
 *
 * @param data IN void pointer to socket
 * @param desc OUT os socket descriptor
 * @return apr status
 */
apr_status_t udp_transport_set_timeout(void *data, apr_interval_time_t t) {
  worker_t *worker = data;
  return apr_socket_timeout_set(worker->socket->socket, t);
}

/**
 * read from socket
 *
 * @param data IN void pointer to socket
 * @param buf IN buffer
 * @param size INOUT buffer len
 * @return apr status
 */
apr_status_t udp_transport_read(void *data, char *buf, apr_size_t *size) {
  worker_t *worker = data;
  return APR_ENOTIMPL; 
}

/**
 * write to socket
 *
 * @param data IN void pointer to socket
 * @param buf IN buffer
 * @param size INOUT buffer len
 * @return apr status
 */
apr_status_t udp_transport_write(void *data, char *buf, apr_size_t size) {
  worker_t *worker = data;
  return APR_ENOTIMPL; 
}

/************************************************************************
 * Commands 
 ***********************************************************************/
/**
 * Udp connect command.
 *
 * @param worker IN worker instance
 * @param parent IN callee
 * @param ptmp IN temp pool for this function
 */
static apr_status_t block_UDP_CONNECT(worker_t *worker, worker_t *parent, 
                                      apr_pool_t *ptmp) {
  apr_status_t status;
  int port;
  apr_sockaddr_t *dest;

  udp_socket_config_t *config = udp_get_socket_config(worker);
  int family = APR_INET;
  char *hostname = store_get_copy(worker->params, ptmp, "1");
  const char *portname = store_get(worker->params, "2");

  if (!hostname) {
    worker_log_error(worker, "No hostname specified");
    return APR_EINVAL;
  }

  if (!portname) {
    worker_log_error(worker, "No port specified");
    return APR_EINVAL;
  }

  /** create udp socket first */
  worker_get_socket(worker, hostname, 
                    apr_pstrcat(ptmp, portname, ":", "udp", NULL));

#if APR_HAVE_IPV6
  /* hostname/address must be surrounded in square brackets */
  if((hostname[0] == '[') && (hostname[strlen(hostname)-1] == ']')) {
    family = APR_INET6;
    hostname++;
    hostname[strlen(hostname)-1] = '\0';
  }
#endif
  if ((status = apr_socket_create(&worker->socket->socket, family,
				  SOCK_DGRAM, APR_PROTO_UDP,
				  worker->pbody)) != APR_SUCCESS) {
    worker->socket->socket = NULL;
    worker_log_error(worker, "Could not create socket");
    return status;
  }

  port = apr_atoi64(portname);

  if ((status = apr_sockaddr_info_get(&dest, hostname, AF_UNSPEC, port,
                                      APR_IPV4_ADDR_OK, worker->pbody))
     != APR_SUCCESS) {
    worker_log_error(worker, "Could not resolve host \"%s\" and port \"%d\"", 
	             hostname, port);
    return status;
  }

  return APR_SUCCESS;
}

/**
 * Udp accept command.
 *
 * @param worker IN worker instance
 * @param parent IN callee
 * @param ptmp IN temp pool for this function
 */
static apr_status_t block_UDP_BIND(worker_t *worker, worker_t *parent, 
                                   apr_pool_t *ptmp) {
  apr_status_t status;
  int port;
  apr_sockaddr_t *dest;

  udp_socket_config_t *config = udp_get_socket_config(worker);
  int family = APR_INET;
  const char *portname = store_get(worker->params, "1");

  if (!portname) {
    worker_log_error(worker, "No port specified");
    return APR_EINVAL;
  }

  /** create udp socket first */
  worker_get_socket(worker, "0.0.0.0", 
                    apr_pstrcat(ptmp, portname, ":", "udp", NULL));

  if ((status = apr_socket_create(&worker->socket->socket, family,
				  SOCK_DGRAM, APR_PROTO_UDP,
				  worker->pbody)) != APR_SUCCESS) {
    worker->socket->socket = NULL;
    worker_log_error(worker, "Could not create socket");
    return status;
  }

  port = apr_atoi64(portname);

  if ((status = apr_sockaddr_info_get(&dest, "0.0.0.0", AF_UNSPEC, port,
                                      APR_IPV4_ADDR_OK, worker->pbody))
     != APR_SUCCESS) {
    worker_log_error(worker, "Could not resolve host port \"%d\"", port);
    return status;
  }

  /** bind to port */
  if ((status = apr_socket_bind(worker->socket->socket, dest)) != APR_SUCCESS) {
    worker_log_error(worker, "Could not bind to host port \"%d\"", port);
    return status;
  }

  return APR_SUCCESS;
}

/************************************************************************
 * Module
 ***********************************************************************/
apr_status_t udp_module_init(global_t *global) {
  apr_status_t status;
  if ((status = module_command_new(global, "UDP", "_CONNECT",
	                           "<ip>:<port>",
	                           "Do connect to a udp destination.",
	                           block_UDP_CONNECT)) != APR_SUCCESS) {
    return status;
  }
  if ((status = module_command_new(global, "UDP", "_BIND",
	                           "<port>",
	                           "Do bind thread to <port>.",
	                           block_UDP_BIND)) != APR_SUCCESS) {
    return status;
  }
  return APR_SUCCESS;
}

