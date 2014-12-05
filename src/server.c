/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#include <qpid/dispatch/ctools.h>
#include <qpid/dispatch/threading.h>
#include <qpid/dispatch/log.h>
#include <qpid/dispatch/amqp.h>
#include "entity.h"
#include "entity_cache.h"
#include "dispatch_private.h"
#include "server_private.h"
#include "timer_private.h"
#include "alloc_private.h"
#include <stdio.h>
#include <time.h>
#include <string.h>

static __thread qd_server_t *thread_server = 0;

ALLOC_DEFINE(qd_work_item_t);
ALLOC_DEFINE(qd_listener_t);
ALLOC_DEFINE(qd_connector_t);
ALLOC_DEFINE(qd_connection_t);
ALLOC_DEFINE(qd_user_fd_t);

const char *QD_CONNECTION_TYPE = "connection";

static const char *conn_state_names[] = {
    "connecting",
    "opening",
    "operational",
    "failed",
    "user"
};
ENUM_DEFINE(conn_state, conn_state_names);

static qd_thread_t *thread(qd_server_t *qd_server, int id)
{
    qd_thread_t *thread = NEW(qd_thread_t);
    if (!thread)
        return 0;

    thread->qd_server    = qd_server;
    thread->thread_id    = id;
    thread->running      = 0;
    thread->canceled     = 0;
    thread->using_thread = 0;

    return thread;
}

qd_error_t qd_entity_update_connection(qd_entity_t* entity, void *impl);

static qd_error_t connection_entity_update_host(qd_entity_t* entity, qd_connection_t *conn)
{
    const qd_server_config_t *config;
    if (conn->connector) {
        config = conn->connector->config;
        char host[strlen(config->host)+strlen(config->port)+2];
        snprintf(host, sizeof(host), "%s:%s", config->host, config->port);
        return qd_entity_set_string(entity, "host", host);
    }
    else
        return qd_entity_set_string(entity, "host", qdpn_connector_name(conn->pn_cxtr));
}

qd_error_t qd_entity_refresh_connection(qd_entity_t* entity, void *impl)
{
    qd_connection_t *conn = (qd_connection_t*)impl;
    const qd_server_config_t *config =
        conn->connector ? conn->connector->config : conn->listener->config;

    if ((qd_entity_has(entity, "identity") ||
         qd_entity_set_string(entity, "identity", qdpn_connector_name(conn->pn_cxtr)) == 0) &&
        qd_entity_set_string(entity, "state", conn_state_name(conn->state)) == 0 &&
        qd_entity_set_string(
            entity, "container",
            conn->pn_conn ? pn_connection_remote_container(conn->pn_conn) : 0) == 0 &&
        connection_entity_update_host(entity, conn) == 0 &&
        /* FIXME aconway 2014-10-14: change attr name to sasl-mechanisms for consistency? */
        qd_entity_set_string(entity, "sasl", config->sasl_mechanisms) == 0 &&
        qd_entity_set_string(entity, "role", config->role) == 0 &&
        qd_entity_set_string(entity, "dir", conn->connector ? "out" : "in") == 0)
        return QD_ERROR_NONE;
    return qd_error_code();
}




static void thread_process_listeners(qd_server_t *qd_server)
{
    qdpn_driver_t     *driver   = qd_server->driver;
    qdpn_listener_t   *listener;
    qdpn_connector_t  *cxtr;
    qd_connection_t *ctx;

    for (listener = qdpn_driver_listener(driver); listener; listener = qdpn_driver_listener(driver)) {
        cxtr = qdpn_listener_accept(listener);
        if (!cxtr)
            continue;

        qd_log(qd_server->log_source, QD_LOG_TRACE, "Accepting Connection from %s", qdpn_connector_name(cxtr));
        ctx = new_qd_connection_t();
        DEQ_ITEM_INIT(ctx);
        ctx->state        = CONN_STATE_OPENING;
        ctx->owner_thread = CONTEXT_NO_OWNER;
        ctx->enqueued     = 0;
        ctx->pn_cxtr      = cxtr;
        ctx->collector    = 0;
        ctx->listener     = (qd_listener_t*) qdpn_listener_context(listener);
        ctx->connector    = 0;
        ctx->context      = ctx->listener->context;
        ctx->user_context = 0;
        ctx->link_context = 0;
        ctx->ufd          = 0;

        size_t clen = strlen(QD_CAPABILITY_ANONYMOUS_RELAY);
        pn_connection_t *conn = pn_connection();
        ctx->collector = pn_collector();
        pn_connection_collect(conn, ctx->collector);
        pn_connection_set_container(conn, qd_server->container_name);
        pn_data_put_symbol(pn_connection_offered_capabilities(conn), pn_bytes(clen, (char*) QD_CAPABILITY_ANONYMOUS_RELAY));
        qdpn_connector_set_connection(cxtr, conn);
        pn_connection_set_context(conn, ctx);
        ctx->pn_conn = conn;

        // qd_server->lock is already locked
        DEQ_INSERT_TAIL(qd_server->connections, ctx);
        qd_entity_cache_add(QD_CONNECTION_TYPE, ctx);

        //
        // Get a pointer to the transport so we can insert security components into it
        //
        pn_transport_t           *tport  = qdpn_connector_transport(cxtr);
        const qd_server_config_t *config = ctx->listener->config;

        //
        // Configure the transport.
        //
        pn_transport_set_max_frame(tport, config->max_frame_size);

        //
        // Set up SSL if appropriate
        //
        if (config->ssl_enabled) {
            pn_ssl_domain_t *domain = pn_ssl_domain(PN_SSL_MODE_SERVER);
            if (!domain) {
                qd_error(QD_ERROR_RUNTIME, "SSL domain failed for connection from %s",
                         qdpn_connector_name(cxtr));
                /* TODO aconway 2014-07-15: Close the connection */
                continue;
            }

            /* TODO aconway 2014-07-15: error handling on all calls. */
            pn_ssl_domain_set_credentials(domain,
                                          config->ssl_certificate_file,
                                          config->ssl_private_key_file,
                                          config->ssl_password);
            if (config->ssl_allow_unsecured_client)
                pn_ssl_domain_allow_unsecured_client(domain);

            if (config->ssl_trusted_certificate_db)
                pn_ssl_domain_set_trusted_ca_db(domain, config->ssl_trusted_certificate_db);

            const char *trusted = config->ssl_trusted_certificate_db;
            if (config->ssl_trusted_certificates)
                trusted = config->ssl_trusted_certificates;

            if (config->ssl_require_peer_authentication)
                pn_ssl_domain_set_peer_authentication(domain, PN_SSL_VERIFY_PEER, trusted);

            pn_ssl_t *ssl = pn_ssl(tport);
            pn_ssl_init(ssl, domain, 0);
            pn_ssl_domain_free(domain);
        }

        //
        // Set up SASL
        //
        pn_sasl_t *sasl = pn_sasl(tport);
        pn_sasl_mechanisms(sasl, config->sasl_mechanisms);
        pn_sasl_server(sasl);
        pn_sasl_allow_skip(sasl, config->allow_no_sasl);
        pn_sasl_done(sasl, PN_SASL_OK);  // TODO - This needs to go away

        qdpn_connector_set_context(cxtr, ctx);
    }
}


static void handle_signals_LH(qd_server_t *qd_server)
{
    int signum = qd_server->pending_signal;

    if (signum) {
        qd_server->pending_signal = 0;
        if (qd_server->signal_handler) {
            sys_mutex_unlock(qd_server->lock);
            qd_server->signal_handler(qd_server->signal_context, signum);
            sys_mutex_lock(qd_server->lock);
        }
    }
}


static void block_if_paused_LH(qd_server_t *qd_server)
{
    if (qd_server->pause_requests > 0) {
        qd_server->threads_paused++;
        sys_cond_signal_all(qd_server->cond);
        while (qd_server->pause_requests > 0)
            sys_cond_wait(qd_server->cond, qd_server->lock);
        qd_server->threads_paused--;
    }
}


static int process_connector(qd_server_t *qd_server, qdpn_connector_t *cxtr)
{
    qd_connection_t *ctx = qdpn_connector_context(cxtr);
    int events = 0;
    int passes = 0;

    if (ctx->state == CONN_STATE_USER) {
        qd_server->ufd_handler(ctx->ufd->context, ctx->ufd);
        return 1;
    }

    do {
        passes++;

        //
        // Step the engine for pre-handler processing
        //
        qdpn_connector_process(cxtr);

        //
        // Call the handler that is appropriate for the connector's state.
        //
        switch (ctx->state) {
        case CONN_STATE_CONNECTING: {
            if (qdpn_connector_closed(cxtr)) {
                ctx->state = CONN_STATE_FAILED;
                events = 0;
                break;
            }

            size_t clen = strlen(QD_CAPABILITY_ANONYMOUS_RELAY);
            pn_connection_t *conn = pn_connection();
            ctx->collector = pn_collector();
            pn_connection_collect(conn, ctx->collector);
            pn_connection_set_container(conn, qd_server->container_name);
            pn_data_put_symbol(pn_connection_offered_capabilities(conn), pn_bytes(clen, (char*) QD_CAPABILITY_ANONYMOUS_RELAY));
            qdpn_connector_set_connection(cxtr, conn);
            pn_connection_set_context(conn, ctx);
            ctx->pn_conn = conn;
            ctx->state   = CONN_STATE_OPENING;
            assert(ctx->connector);
            ctx->connector->state = CXTR_STATE_OPEN;
            events = 1;
            break;
        }

        case CONN_STATE_OPENING: {
            pn_transport_t *tport = qdpn_connector_transport(cxtr);
            pn_sasl_t      *sasl  = pn_sasl(tport);

            if (pn_sasl_outcome(sasl) == PN_SASL_OK ||
                pn_sasl_outcome(sasl) == PN_SASL_SKIPPED) {
                ctx->state = CONN_STATE_OPERATIONAL;

                qd_conn_event_t ce = QD_CONN_EVENT_PROCESS; // Initialize to keep the compiler happy

                if (ctx->listener) {
                    ce = QD_CONN_EVENT_LISTENER_OPEN;
                } else if (ctx->connector) {
                    ce = QD_CONN_EVENT_CONNECTOR_OPEN;
                    ctx->connector->delay = 0;
                } else
                    assert(0);

                qd_server->conn_handler(qd_server->conn_handler_context,
                                        ctx->context, ce, (qd_connection_t*) qdpn_connector_context(cxtr));
                events = 1;
                break;
            }
            else if (pn_sasl_outcome(sasl) != PN_SASL_NONE) {
                ctx->state = CONN_STATE_FAILED;
                if (ctx->connector) {
                    const qd_server_config_t *config = ctx->connector->config;
                    qd_log(qd_server->log_source, QD_LOG_TRACE, "Connection to %s:%s failed", config->host, config->port);
                }
            }
        }

        case CONN_STATE_OPERATIONAL:
            if (qdpn_connector_closed(cxtr)) {
                qd_server->conn_handler(qd_server->conn_handler_context, ctx->context,
                                        QD_CONN_EVENT_CLOSE,
                                        (qd_connection_t*) qdpn_connector_context(cxtr));
                events = 0;
            }
            else
                events = qd_server->conn_handler(qd_server->conn_handler_context, ctx->context,
                                                 QD_CONN_EVENT_PROCESS,
                                                 (qd_connection_t*) qdpn_connector_context(cxtr));
            break;

        default:
            break;
        }
    } while (events > 0);

    return passes > 1;
}


//
// TEMPORARY FUNCTION PROTOTYPES
//
void qdpn_driver_wait_1(qdpn_driver_t *d);
int  qdpn_driver_wait_2(qdpn_driver_t *d, int timeout);
void qdpn_driver_wait_3(qdpn_driver_t *d);
//
// END TEMPORARY
//

static void *thread_run(void *arg)
{
    qd_thread_t     *thread    = (qd_thread_t*) arg;
    qd_server_t     *qd_server = thread->qd_server;
    qd_work_item_t  *work;
    qdpn_connector_t  *cxtr;
    pn_connection_t *conn;
    qd_connection_t *ctx;
    int              error;
    int              poll_result;

    if (!thread)
        return 0;

    thread_server   = qd_server;
    thread->running = 1;

    if (thread->canceled)
        return 0;

    //
    // Invoke the start handler if the application supplied one.
    // This handler can be used to set NUMA or processor affinnity for the thread.
    //
    if (qd_server->start_handler)
        qd_server->start_handler(qd_server->start_context, thread->thread_id);

    //
    // Main Loop
    //
    while (thread->running) {
        sys_mutex_lock(qd_server->lock);

        //
        // Check for pending signals to process
        //
        handle_signals_LH(qd_server);
        if (!thread->running) {
            sys_mutex_unlock(qd_server->lock);
            break;
        }

        //
        // Check to see if the server is pausing.  If so, block here.
        //
        block_if_paused_LH(qd_server);
        if (!thread->running) {
            sys_mutex_unlock(qd_server->lock);
            break;
        }

        //
        // Service pending timers.
        //
        qd_timer_t *timer = DEQ_HEAD(qd_server->pending_timers);
        if (timer) {
            DEQ_REMOVE_HEAD(qd_server->pending_timers);

            //
            // Mark the timer as idle in case it reschedules itself.
            //
            qd_timer_idle_LH(timer);

            //
            // Release the lock and invoke the connection handler.
            //
            sys_mutex_unlock(qd_server->lock);
            timer->handler(timer->context);
            qdpn_driver_wakeup(qd_server->driver);
            continue;
        }

        //
        // Check the work queue for connectors scheduled for processing.
        //
        work = DEQ_HEAD(qd_server->work_queue);
        if (!work) {
            //
            // There is no pending work to do
            //
            if (qd_server->a_thread_is_waiting) {
                //
                // Another thread is waiting on the proton driver, this thread must
                // wait on the condition variable until signaled.
                //
                sys_cond_wait(qd_server->cond, qd_server->lock);
            } else {
                //
                // This thread elects itself to wait on the proton driver.  Set the
                // thread-is-waiting flag so other idle threads will not interfere.
                //
                qd_server->a_thread_is_waiting = true;

                //
                // Ask the timer module when its next timer is scheduled to fire.  We'll
                // use this value in driver_wait as the timeout.  If there are no scheduled
                // timers, the returned value will be -1.
                //
                long duration = qd_timer_next_duration_LH();

                //
                // Invoke the proton driver's wait sequence.  This is a bit of a hack for now
                // and will be improved in the future.  The wait process is divided into three parts,
                // the first and third of which need to be non-reentrant, and the second of which
                // must be reentrant (and blocks).
                //
                qdpn_driver_wait_1(qd_server->driver);
                sys_mutex_unlock(qd_server->lock);

                do {
                    error = 0;
                    poll_result = qdpn_driver_wait_2(qd_server->driver, duration);
                    if (poll_result == -1)
                        error = qdpn_driver_errno(qd_server->driver);
                } while (error == PN_INTR);
                if (error) {
                    qd_log(qd_server->log_source, QD_LOG_ERROR, "Driver Error: %s", qdpn_driver_error(qd_server->driver));
                    exit(-1);
                }

                sys_mutex_lock(qd_server->lock);
                qdpn_driver_wait_3(qd_server->driver);

                if (!thread->running) {
                    sys_mutex_unlock(qd_server->lock);
                    break;
                }

                //
                // Visit the timer module.
                //
                struct timespec tv;
                clock_gettime(CLOCK_REALTIME, &tv);
                long milliseconds = tv.tv_sec * 1000 + tv.tv_nsec / 1000000;
                qd_timer_visit_LH(milliseconds);

                //
                // Process listeners (incoming connections).
                //
                thread_process_listeners(qd_server);

                //
                // Traverse the list of connectors-needing-service from the proton driver.
                // If the connector is not already in the work queue and it is not currently
                // being processed by another thread, put it in the work queue and signal the
                // condition variable.
                //
                cxtr = qdpn_driver_connector(qd_server->driver);
                while (cxtr) {
                    ctx = qdpn_connector_context(cxtr);
                    if (!ctx->enqueued && ctx->owner_thread == CONTEXT_NO_OWNER) {
                        ctx->enqueued = 1;
                        qd_work_item_t *workitem = new_qd_work_item_t();
                        DEQ_ITEM_INIT(workitem);
                        workitem->cxtr = cxtr;
                        DEQ_INSERT_TAIL(qd_server->work_queue, workitem);
                        sys_cond_signal(qd_server->cond);
                    }
                    cxtr = qdpn_driver_connector(qd_server->driver);
                }

                //
                // Release our exclusive claim on qdpn_driver_wait.
                //
                qd_server->a_thread_is_waiting = false;
            }
        }

        //
        // If we were given a connector to work on from the work queue, mark it as
        // owned by this thread and as no longer enqueued.
        //
        cxtr = 0;
        if (work) {
            DEQ_REMOVE_HEAD(qd_server->work_queue);
            ctx = qdpn_connector_context(work->cxtr);
            if (ctx->owner_thread == CONTEXT_NO_OWNER) {
                ctx->owner_thread = thread->thread_id;
                ctx->enqueued = 0;
                qd_server->threads_active++;
                cxtr = work->cxtr;
                free_qd_work_item_t(work);
            } else {
                //
                // This connector is being processed by another thread, re-queue it.
                //
                DEQ_INSERT_TAIL(qd_server->work_queue, work);
            }
        }
        sys_mutex_unlock(qd_server->lock);

        //
        // Process the connector that we now have exclusive access to.
        //
        if (cxtr) {
            int work_done = 1;

            if (qdpn_connector_failed(cxtr))
                qdpn_connector_close(cxtr);
            else
                work_done = process_connector(qd_server, cxtr);

            //
            // Check to see if the connector was closed during processing
            //
            if (qdpn_connector_closed(cxtr)) {
                qd_entity_cache_remove(QD_CONNECTION_TYPE, ctx);
                //
                // Connector is closed.  Free the context and the connector.
                //
                conn = qdpn_connector_connection(cxtr);

                //
                // If this is a dispatch connector, schedule the re-connect timer
                //
                if (ctx->connector) {
                    ctx->connector->ctx = 0;
                    ctx->connector->state = CXTR_STATE_CONNECTING;
                    qd_timer_schedule(ctx->connector->timer, ctx->connector->delay);
                }

                sys_mutex_lock(qd_server->lock);
                DEQ_REMOVE(qd_server->connections, ctx);

                qdpn_connector_free(cxtr);
                if (conn)
                    pn_connection_free(conn);
                if (ctx->collector)
                    pn_collector_free(ctx->collector);
                free_qd_connection_t(ctx);
                qd_server->threads_active--;
                sys_mutex_unlock(qd_server->lock);
            } else {
                //
                // The connector lives on.  Mark it as no longer owned by this thread.
                //
                sys_mutex_lock(qd_server->lock);
                ctx->owner_thread = CONTEXT_NO_OWNER;
                qd_server->threads_active--;
                sys_mutex_unlock(qd_server->lock);
            }

            //
            // Wake up the proton driver to force it to reconsider its set of FDs
            // in light of the processing that just occurred.
            //
            if (work_done)
                qdpn_driver_wakeup(qd_server->driver);
        }
    }

    return 0;
}


static void thread_start(qd_thread_t *thread)
{
    if (!thread)
        return;

    thread->using_thread = 1;
    thread->thread = sys_thread(thread_run, (void*) thread);
}


static void thread_cancel(qd_thread_t *thread)
{
    if (!thread)
        return;

    thread->running  = 0;
    thread->canceled = 1;
}


static void thread_join(qd_thread_t *thread)
{
    if (!thread)
        return;

    if (thread->using_thread) {
        sys_thread_join(thread->thread);
        sys_thread_free(thread->thread);
    }
}


static void thread_free(qd_thread_t *thread)
{
    if (!thread)
        return;

    free(thread);
}


static void cxtr_try_open(void *context)
{
    qd_connector_t *ct = (qd_connector_t*) context;
    if (ct->state != CXTR_STATE_CONNECTING)
        return;

    qd_connection_t *ctx = new_qd_connection_t();
    DEQ_ITEM_INIT(ctx);
    ctx->server       = ct->server;
    ctx->state        = CONN_STATE_CONNECTING;
    ctx->owner_thread = CONTEXT_NO_OWNER;
    ctx->enqueued     = 0;
    ctx->pn_conn      = 0;
    ctx->collector    = 0;
    ctx->listener     = 0;
    ctx->connector    = ct;
    ctx->context      = ct->context;
    ctx->user_context = 0;
    ctx->link_context = 0;
    ctx->ufd          = 0;

    //
    // qdpn_connector is not thread safe
    //
    sys_mutex_lock(ct->server->lock);
    ctx->pn_cxtr = qdpn_connector(ct->server->driver, ct->config->host, ct->config->port, (void*) ctx);
    DEQ_INSERT_TAIL(ct->server->connections, ctx);
    qd_entity_cache_add(QD_CONNECTION_TYPE, ctx);

    sys_mutex_unlock(ct->server->lock);

    ct->ctx   = ctx;
    ct->delay = 5000;
    qd_log(ct->server->log_source, QD_LOG_TRACE, "Connecting to %s:%s", ct->config->host, ct->config->port);

    //
    // Set up the transport, SASL, and SSL for the connection.
    //
    pn_transport_t           *tport  = qdpn_connector_transport(ctx->pn_cxtr);
    const qd_server_config_t *config = ct->config;

    //
    // Configure the transport
    //
    pn_transport_set_max_frame(tport, config->max_frame_size);

    //
    // Set up SSL if appropriate
    //
    if (config->ssl_enabled) {
        pn_ssl_domain_t *domain = pn_ssl_domain(PN_SSL_MODE_CLIENT);
        if (!domain) {
            qd_error(QD_ERROR_RUNTIME, "SSL domain failed for connection to %s:%s",
                     ct->config->host, ct->config->port);
            /* TODO aconway 2014-07-15: Close the connection, clean up. */
            return;
        }
        /* TODO aconway 2014-07-15: error handling on all SSL calls. */
        pn_ssl_domain_set_credentials(domain,
                                      config->ssl_certificate_file,
                                      config->ssl_private_key_file,
                                      config->ssl_password);

        if (config->ssl_require_peer_authentication)
            pn_ssl_domain_set_peer_authentication(domain, PN_SSL_VERIFY_PEER, config->ssl_trusted_certificate_db);

        pn_ssl_t *ssl = pn_ssl(tport);
        pn_ssl_init(ssl, domain, 0);
        pn_ssl_domain_free(domain);
    }

    //
    // Set up SASL
    //
    pn_sasl_t *sasl = pn_sasl(tport);
    pn_sasl_mechanisms(sasl, config->sasl_mechanisms);
    pn_sasl_client(sasl);
}


qd_server_t *qd_server(qd_dispatch_t *qd, int thread_count, const char *container_name)
{
    int i;

    qd_server_t *qd_server = NEW(qd_server_t);
    if (qd_server == 0)
        return 0;

    DEQ_INIT(qd_server->connections);
    qd_server->qd              = qd;
    qd_server->log_source      = qd_log_source("SERVER");
    qd_server->thread_count    = thread_count;
    qd_server->container_name  = container_name;
    qd_server->driver          = qdpn_driver();
    qd_server->start_handler   = 0;
    qd_server->conn_handler    = 0;
    qd_server->signal_handler  = 0;
    qd_server->ufd_handler     = 0;
    qd_server->start_context   = 0;
    qd_server->signal_context  = 0;
    qd_server->lock            = sys_mutex();
    qd_server->cond            = sys_cond();

    qd_timer_initialize(qd_server->lock);

    qd_server->threads = NEW_PTR_ARRAY(qd_thread_t, thread_count);
    for (i = 0; i < thread_count; i++)
        qd_server->threads[i] = thread(qd_server, i);

    DEQ_INIT(qd_server->work_queue);
    DEQ_INIT(qd_server->pending_timers);
    qd_server->a_thread_is_waiting = false;
    qd_server->threads_active      = 0;
    qd_server->pause_requests      = 0;
    qd_server->threads_paused      = 0;
    qd_server->pause_next_sequence = 0;
    qd_server->pause_now_serving   = 0;
    qd_server->pending_signal      = 0;

    qd_log(qd_server->log_source, QD_LOG_INFO, "Container Name: %s", qd_server->container_name);

    return qd_server;
}


void qd_server_free(qd_server_t *qd_server)
{
    if (!qd_server) return;
    int i;
    if (!qd_server)
        return;

    for (i = 0; i < qd_server->thread_count; i++)
        thread_free(qd_server->threads[i]);

    qdpn_driver_free(qd_server->driver);
    sys_mutex_free(qd_server->lock);
    sys_cond_free(qd_server->cond);
    free(qd_server->threads);
    free(qd_server);
}


void qd_server_set_conn_handler(qd_dispatch_t *qd, qd_conn_handler_cb_t handler, void *handler_context)
{
    qd->server->conn_handler         = handler;
    qd->server->conn_handler_context = handler_context;
}


void qd_server_set_signal_handler(qd_dispatch_t *qd, qd_signal_handler_cb_t handler, void *context)
{
    qd->server->signal_handler = handler;
    qd->server->signal_context = context;
}


void qd_server_set_start_handler(qd_dispatch_t *qd, qd_thread_start_cb_t handler, void *context)
{
    qd->server->start_handler = handler;
    qd->server->start_context = context;
}


void qd_server_set_user_fd_handler(qd_dispatch_t *qd, qd_user_fd_handler_cb_t ufd_handler)
{
    qd->server->ufd_handler = ufd_handler;
}


static void qd_server_announce(qd_server_t* qd_server)
{
    qd_log(qd_server->log_source, QD_LOG_INFO, "Operational, %d Threads Running", qd_server->thread_count);
#ifndef NDEBUG
    qd_log(qd_server->log_source, QD_LOG_INFO, "Running in DEBUG Mode");
#endif
}


void qd_server_run(qd_dispatch_t *qd)
{
    qd_server_t *qd_server = qd->server;

    int i;
    if (!qd_server)
        return;

    assert(qd_server->conn_handler); // Server can't run without a connection handler.

    for (i = 1; i < qd_server->thread_count; i++)
        thread_start(qd_server->threads[i]);

    qd_server_announce(qd_server);

    thread_run((void*) qd_server->threads[0]);

    for (i = 1; i < qd_server->thread_count; i++)
        thread_join(qd_server->threads[i]);

    for (i = 0; i < qd_server->thread_count; i++)
        qd_server->threads[i]->canceled = 0;

    qd_log(qd_server->log_source, QD_LOG_INFO, "Shut Down");
}


void qd_server_start(qd_dispatch_t *qd)
{
    qd_server_t *qd_server = qd->server;
    int i;

    if (!qd_server)
        return;

    assert(qd_server->conn_handler); // Server can't run without a connection handler.

    for (i = 0; i < qd_server->thread_count; i++)
        thread_start(qd_server->threads[i]);

    qd_server_announce(qd_server);
}


void qd_server_stop(qd_dispatch_t *qd)
{
    qd_server_t *qd_server = qd->server;
    int idx;

    sys_mutex_lock(qd_server->lock);
    for (idx = 0; idx < qd_server->thread_count; idx++)
        thread_cancel(qd_server->threads[idx]);
    sys_cond_signal_all(qd_server->cond);
    qdpn_driver_wakeup(qd_server->driver);
    sys_mutex_unlock(qd_server->lock);

    if (thread_server != qd_server) {
        for (idx = 0; idx < qd_server->thread_count; idx++)
            thread_join(qd_server->threads[idx]);
        qd_log(qd_server->log_source, QD_LOG_INFO, "Shut Down");
    }
}


void qd_server_signal(qd_dispatch_t *qd, int signum)
{
    qd_server_t *qd_server = qd->server;

    qd_server->pending_signal = signum;
    sys_cond_signal_all(qd_server->cond);
    qdpn_driver_wakeup(qd_server->driver);
}


void qd_server_pause(qd_dispatch_t *qd)
{
    qd_server_t *qd_server = qd->server;

    sys_mutex_lock(qd_server->lock);

    //
    // Bump the request count to stop all the threads.
    //
    qd_server->pause_requests++;
    int my_sequence = qd_server->pause_next_sequence++;

    //
    // Awaken all threads that are currently blocking.
    //
    sys_cond_signal_all(qd_server->cond);
    qdpn_driver_wakeup(qd_server->driver);

    //
    // Wait for the paused thread count plus the number of threads requesting a pause to equal
    // the total thread count.  Also, don't exit the blocking loop until now_serving equals our
    // sequence number.  This ensures that concurrent pausers don't run at the same time.
    //
    while ((qd_server->threads_paused + qd_server->pause_requests < qd_server->thread_count) ||
           (my_sequence != qd_server->pause_now_serving))
        sys_cond_wait(qd_server->cond, qd_server->lock);

    sys_mutex_unlock(qd_server->lock);
}


void qd_server_resume(qd_dispatch_t *qd)
{
    qd_server_t *qd_server = qd->server;

    sys_mutex_lock(qd_server->lock);
    qd_server->pause_requests--;
    qd_server->pause_now_serving++;
    sys_cond_signal_all(qd_server->cond);
    sys_mutex_unlock(qd_server->lock);
}


void qd_server_activate(qd_connection_t *ctx)
{
    if (!ctx)
        return;

    qdpn_connector_t *ctor = ctx->pn_cxtr;
    if (!ctor)
        return;

    if (!qdpn_connector_closed(ctor))
        qdpn_connector_activate(ctor, QDPN_CONNECTOR_WRITABLE);
}


void qd_connection_set_context(qd_connection_t *conn, void *context)
{
    conn->user_context = context;
}


void *qd_connection_get_context(qd_connection_t *conn)
{
    return conn->user_context;
}


void qd_connection_set_link_context(qd_connection_t *conn, void *context)
{
    conn->link_context = context;
}


void *qd_connection_get_link_context(qd_connection_t *conn)
{
    return conn->link_context;
}


pn_connection_t *qd_connection_pn(qd_connection_t *conn)
{
    return conn->pn_conn;
}


pn_collector_t *qd_connection_collector(qd_connection_t *conn)
{
    return conn->collector;
}


const qd_server_config_t *qd_connection_config(const qd_connection_t *conn)
{
    if (conn->listener)
        return conn->listener->config;
    return conn->connector->config;
}


qd_listener_t *qd_server_listen(qd_dispatch_t *qd, const qd_server_config_t *config, void *context)
{
    qd_server_t   *qd_server = qd->server;
    qd_listener_t *li        = new_qd_listener_t();

    if (!li)
        return 0;

    li->server      = qd_server;
    li->config      = config;
    li->context     = context;
    li->pn_listener = qdpn_listener(qd_server->driver, config->host, config->port, (void*) li);

    if (!li->pn_listener) {
        qd_log(qd_server->log_source, QD_LOG_ERROR, "Driver Error %d (%s)",
               qdpn_driver_errno(qd_server->driver), qdpn_driver_error(qd_server->driver));
        free_qd_listener_t(li);
        return 0;
    }
    qd_log(qd_server->log_source, QD_LOG_TRACE, "Listening on %s:%s", config->host, config->port);

    return li;
}


void qd_server_listener_free(qd_listener_t* li)
{
    if (!li)
        return;

    qdpn_listener_free(li->pn_listener);
    free_qd_listener_t(li);
}


void qd_server_listener_close(qd_listener_t* li)
{
    qdpn_listener_close(li->pn_listener);
}


qd_connector_t *qd_server_connect(qd_dispatch_t *qd, const qd_server_config_t *config, void *context)
{
    qd_server_t    *qd_server = qd->server;
    qd_connector_t *ct        = new_qd_connector_t();

    if (!ct)
        return 0;

    ct->server  = qd_server;
    ct->state   = CXTR_STATE_CONNECTING;
    ct->config  = config;
    ct->context = context;
    ct->ctx     = 0;
    ct->timer   = qd_timer(qd, cxtr_try_open, (void*) ct);
    ct->delay   = 0;

    qd_timer_schedule(ct->timer, ct->delay);
    return ct;
}


void qd_server_connector_free(qd_connector_t* ct)
{
    // Don't free the proton connector.  This will be done by the connector
    // processing/cleanup.

    if (!ct)
        return;

    if (ct->ctx) {
        qdpn_connector_close(ct->ctx->pn_cxtr);
        ct->ctx->connector = 0;
    }

    qd_timer_free(ct->timer);
    free_qd_connector_t(ct);
}


qd_user_fd_t *qd_user_fd(qd_dispatch_t *qd, int fd, void *context)
{
    qd_server_t  *qd_server = qd->server;
    qd_user_fd_t *ufd       = new_qd_user_fd_t();

    if (!ufd)
        return 0;

    qd_connection_t *ctx = new_qd_connection_t();
    DEQ_ITEM_INIT(ctx);
    ctx->server       = qd_server;
    ctx->state        = CONN_STATE_USER;
    ctx->owner_thread = CONTEXT_NO_OWNER;
    ctx->enqueued     = 0;
    ctx->pn_conn      = 0;
    ctx->collector    = 0;
    ctx->listener     = 0;
    ctx->connector    = 0;
    ctx->context      = 0;
    ctx->user_context = 0;
    ctx->link_context = 0;
    ctx->ufd          = ufd;

    ufd->context = context;
    ufd->server  = qd_server;
    ufd->fd      = fd;
    ufd->pn_conn = qdpn_connector_fd(qd_server->driver, fd, (void*) ctx);
    qdpn_driver_wakeup(qd_server->driver);

    return ufd;
}


void qd_user_fd_free(qd_user_fd_t *ufd)
{
    if (!ufd) return;
    qdpn_connector_close(ufd->pn_conn);
    free_qd_user_fd_t(ufd);
}


void qd_user_fd_activate_read(qd_user_fd_t *ufd)
{
    qdpn_connector_activate(ufd->pn_conn, QDPN_CONNECTOR_READABLE);
    qdpn_driver_wakeup(ufd->server->driver);
}


void qd_user_fd_activate_write(qd_user_fd_t *ufd)
{
    qdpn_connector_activate(ufd->pn_conn, QDPN_CONNECTOR_WRITABLE);
    qdpn_driver_wakeup(ufd->server->driver);
}


bool qd_user_fd_is_readable(qd_user_fd_t *ufd)
{
    return qdpn_connector_activated(ufd->pn_conn, QDPN_CONNECTOR_READABLE);
}


bool qd_user_fd_is_writeable(qd_user_fd_t *ufd)
{
    return qdpn_connector_activated(ufd->pn_conn, QDPN_CONNECTOR_WRITABLE);
}


void qd_server_timer_pending_LH(qd_timer_t *timer)
{
    DEQ_INSERT_TAIL(timer->server->pending_timers, timer);
}


void qd_server_timer_cancel_LH(qd_timer_t *timer)
{
    DEQ_REMOVE(timer->server->pending_timers, timer);
}
