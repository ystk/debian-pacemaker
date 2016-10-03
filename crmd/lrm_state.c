/* 
 * Copyright (C) 2012 David Vossel <davidvossel@gmail.com>
 * 
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 * 
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include <crm_internal.h>
#include <crm/crm.h>
#include <crm/msg_xml.h>

#include <crmd.h>
#include <crmd_fsa.h>
#include <crmd_messages.h>
#include <crmd_callbacks.h>
#include <crmd_lrm.h>

GHashTable *lrm_state_table = NULL;
extern GHashTable *proxy_table;
int lrmd_internal_proxy_send(lrmd_t * lrmd, xmlNode *msg);
void lrmd_internal_set_proxy_callback(lrmd_t * lrmd, void *userdata, void (*callback)(lrmd_t *lrmd, void *userdata, xmlNode *msg));

static void
free_rsc_info(gpointer value)
{
    lrmd_rsc_info_t *rsc_info = value;

    lrmd_free_rsc_info(rsc_info);
}

static void
free_deletion_op(gpointer value)
{
    struct pending_deletion_op_s *op = value;

    free(op->rsc);
    delete_ha_msg_input(op->input);
    free(op);
}

static void
free_recurring_op(gpointer value)
{
    struct recurring_op_s *op = (struct recurring_op_s *)value;

    free(op->user_data);
    free(op->rsc_id);
    free(op->op_type);
    free(op->op_key);
    if (op->params) {
        g_hash_table_destroy(op->params);
    }
    free(op);
}

static gboolean
fail_pending_op(gpointer key, gpointer value, gpointer user_data)
{
    lrmd_event_data_t event = { 0, };
    lrm_state_t *lrm_state = user_data;
    struct recurring_op_s *op = (struct recurring_op_s *)value;

    crm_trace("Pre-emptively failing %s_%s_%d on %s (call=%s, %s)",
              op->rsc_id, op->op_type, op->interval,
              lrm_state->node_name, key, op->user_data);

    event.type = lrmd_event_exec_complete;
    event.rsc_id = op->rsc_id;
    event.op_type = op->op_type;
    event.user_data = op->user_data;
    event.timeout = 0;
    event.interval = op->interval;
    event.rc = PCMK_OCF_CONNECTION_DIED;
    event.op_status = PCMK_LRM_OP_ERROR;
    event.t_run = op->start_time;
    event.t_rcchange = op->start_time;

    event.call_id = op->call_id;
    event.remote_nodename = lrm_state->node_name;
    event.params = op->params;

    process_lrm_event(lrm_state, &event, op);
    return TRUE;
}

gboolean
lrm_state_is_local(lrm_state_t *lrm_state)
{
    if (lrm_state == NULL || fsa_our_uname == NULL) {
        return FALSE;
    }

    if (strcmp(lrm_state->node_name, fsa_our_uname) != 0) {
        return FALSE;
    }

    return TRUE;

}

lrm_state_t *
lrm_state_create(const char *node_name)
{
    lrm_state_t *state = NULL;

    if (!node_name) {
        crm_err("No node name given for lrm state object");
        return NULL;
    }

    state = calloc(1, sizeof(lrm_state_t));
    if (!state) {
        return NULL;
    }

    state->node_name = strdup(node_name);

    state->rsc_info_cache = g_hash_table_new_full(crm_str_hash,
                                                g_str_equal, NULL, free_rsc_info);

    state->deletion_ops = g_hash_table_new_full(crm_str_hash,
                                                g_str_equal, g_hash_destroy_str, free_deletion_op);

    state->pending_ops = g_hash_table_new_full(crm_str_hash,
                                               g_str_equal, g_hash_destroy_str, free_recurring_op);

    state->resource_history = g_hash_table_new_full(crm_str_hash,
                                                    g_str_equal, NULL, history_free);

    g_hash_table_insert(lrm_state_table, (char *)state->node_name, state);
    return state;

}

void
lrm_state_destroy(const char *node_name)
{
    g_hash_table_remove(lrm_state_table, node_name);
}

static gboolean
remote_proxy_remove_by_node(gpointer key, gpointer value, gpointer user_data)
{
    remote_proxy_t *proxy = value;
    const char *node_name = user_data;

    if (safe_str_eq(node_name, proxy->node_name)) {
        return TRUE;
    }

    return FALSE;
}

static void
internal_lrm_state_destroy(gpointer data)
{
    lrm_state_t *lrm_state = data;

    if (!lrm_state) {
        return;
    }

    crm_trace("Destroying proxy table %s with %d members", lrm_state->node_name, g_hash_table_size(proxy_table));
    g_hash_table_foreach_remove(proxy_table, remote_proxy_remove_by_node, (char *) lrm_state->node_name);
    remote_ra_cleanup(lrm_state);
    lrmd_api_delete(lrm_state->conn);

    if (lrm_state->rsc_info_cache) {
        crm_trace("Destroying rsc info cache with %d members", g_hash_table_size(lrm_state->rsc_info_cache));
        g_hash_table_destroy(lrm_state->rsc_info_cache);
    }
    if (lrm_state->resource_history) {
        crm_trace("Destroying history op cache with %d members", g_hash_table_size(lrm_state->resource_history));
        g_hash_table_destroy(lrm_state->resource_history);
    }
    if (lrm_state->deletion_ops) {
        crm_trace("Destroying deletion op cache with %d members", g_hash_table_size(lrm_state->deletion_ops));
        g_hash_table_destroy(lrm_state->deletion_ops);
    }
    if (lrm_state->pending_ops) {
        crm_trace("Destroying pending op cache with %d members", g_hash_table_size(lrm_state->pending_ops));
        g_hash_table_destroy(lrm_state->pending_ops);
    }

    free((char *)lrm_state->node_name);
    free(lrm_state);
}

void
lrm_state_reset_tables(lrm_state_t * lrm_state)
{
    if (lrm_state->resource_history) {
        crm_trace("Re-setting history op cache with %d members",
                  g_hash_table_size(lrm_state->resource_history));
        g_hash_table_remove_all(lrm_state->resource_history);
    }
    if (lrm_state->deletion_ops) {
        crm_trace("Re-setting deletion op cache with %d members",
                  g_hash_table_size(lrm_state->deletion_ops));
        g_hash_table_remove_all(lrm_state->deletion_ops);
    }
    if (lrm_state->pending_ops) {
        crm_trace("Re-setting pending op cache with %d members",
                  g_hash_table_size(lrm_state->pending_ops));
        g_hash_table_remove_all(lrm_state->pending_ops);
    }
    if (lrm_state->rsc_info_cache) {
        crm_trace("Re-setting rsc info cache with %d members",
                  g_hash_table_size(lrm_state->rsc_info_cache));
        g_hash_table_remove_all(lrm_state->rsc_info_cache);
    }
}

gboolean
lrm_state_init_local(void)
{
    if (lrm_state_table) {
        return TRUE;
    }

    lrm_state_table =
        g_hash_table_new_full(crm_strcase_hash, crm_strcase_equal, NULL, internal_lrm_state_destroy);
    if (!lrm_state_table) {
        return FALSE;
    }

    proxy_table =
        g_hash_table_new_full(crm_strcase_hash, crm_strcase_equal, NULL, remote_proxy_free);
    if (!proxy_table) {
         g_hash_table_destroy(lrm_state_table);
        return FALSE;
    }

    return TRUE;
}

void
lrm_state_destroy_all(void)
{
    if (lrm_state_table) {
        crm_trace("Destroying state table with %d members", g_hash_table_size(lrm_state_table));
        g_hash_table_destroy(lrm_state_table); lrm_state_table = NULL;
    }
    if(proxy_table) {
        crm_trace("Destroying proxy table with %d members", g_hash_table_size(proxy_table));
        g_hash_table_destroy(proxy_table); proxy_table = NULL;
    }
}

lrm_state_t *
lrm_state_find(const char *node_name)
{
    if (!node_name) {
        return NULL;
    }
    return g_hash_table_lookup(lrm_state_table, node_name);
}

lrm_state_t *
lrm_state_find_or_create(const char *node_name)
{
    lrm_state_t *lrm_state;

    lrm_state = g_hash_table_lookup(lrm_state_table, node_name);
    if (!lrm_state) {
        lrm_state = lrm_state_create(node_name);
    }

    return lrm_state;
}

GList *
lrm_state_get_list(void)
{
    return g_hash_table_get_values(lrm_state_table);
}

void
lrm_state_disconnect(lrm_state_t * lrm_state)
{
    int removed = 0;

    if (!lrm_state->conn) {
        return;
    }
    crm_trace("Disconnecting %s", lrm_state->node_name);
    ((lrmd_t *) lrm_state->conn)->cmds->disconnect(lrm_state->conn);

    if (is_not_set(fsa_input_register, R_SHUTDOWN)) {
        removed = g_hash_table_foreach_remove(lrm_state->pending_ops, fail_pending_op, lrm_state);
        crm_trace("Synthesized %d operation failures for %s", removed, lrm_state->node_name);
    }

    lrmd_api_delete(lrm_state->conn);
    lrm_state->conn = NULL;
}

int
lrm_state_is_connected(lrm_state_t * lrm_state)
{
    if (!lrm_state->conn) {
        return FALSE;
    }
    return ((lrmd_t *) lrm_state->conn)->cmds->is_connected(lrm_state->conn);
}

int
lrm_state_poke_connection(lrm_state_t * lrm_state)
{

    if (!lrm_state->conn) {
        return -1;
    }
    return ((lrmd_t *) lrm_state->conn)->cmds->poke_connection(lrm_state->conn);
}

int
lrm_state_ipc_connect(lrm_state_t * lrm_state)
{
    int ret;

    if (!lrm_state->conn) {
        lrm_state->conn = lrmd_api_new();
        ((lrmd_t *) lrm_state->conn)->cmds->set_callback(lrm_state->conn, lrm_op_callback);
    }

    ret = ((lrmd_t *) lrm_state->conn)->cmds->connect(lrm_state->conn, CRM_SYSTEM_CRMD, NULL);

    if (ret != pcmk_ok) {
        lrm_state->num_lrm_register_fails++;
    } else {
        lrm_state->num_lrm_register_fails = 0;
    }

    return ret;
}

static int
remote_proxy_dispatch_internal(const char *buffer, ssize_t length, gpointer userdata)
{
    /* Async responses from cib and friends back to clients via pacemaker_remoted */
    xmlNode *xml = NULL;
    remote_proxy_t *proxy = userdata;
    lrm_state_t *lrm_state = lrm_state_find(proxy->node_name);
    uint32_t flags;

    if (lrm_state == NULL) {
        return 0;
    }

    xml = string2xml(buffer);
    if (xml == NULL) {
        crm_warn("Received a NULL msg from IPC service.");
        return 1;
    }

    flags = crm_ipc_buffer_flags(proxy->ipc);
    if (flags & crm_ipc_proxied_relay_response) {
        crm_trace("Passing response back to %.8s on %s: %.200s - request id: %d", proxy->session_id, proxy->node_name, buffer, proxy->last_request_id);
        remote_proxy_relay_response(lrm_state->conn, proxy->session_id, xml, proxy->last_request_id);
        proxy->last_request_id = 0;

    } else {
        crm_trace("Passing event back to %.8s on %s: %.200s", proxy->session_id, proxy->node_name, buffer);
        remote_proxy_relay_event(lrm_state->conn, proxy->session_id, xml);
    }
    free_xml(xml);
    return 1;
}

static void
remote_proxy_disconnected(void *userdata)
{
    remote_proxy_t *proxy = userdata;
    lrm_state_t *lrm_state = lrm_state_find(proxy->node_name);

    crm_trace("Destroying %s (%p)", lrm_state->node_name, userdata);

    proxy->source = NULL;
    proxy->ipc = NULL;

    if (lrm_state && lrm_state->conn) {
        remote_proxy_notify_destroy(lrm_state->conn, proxy->session_id);
    }
    g_hash_table_remove(proxy_table, proxy->session_id);
}

static remote_proxy_t *
remote_proxy_new(const char *node_name, const char *session_id, const char *channel)
{
    static struct ipc_client_callbacks proxy_callbacks = {
        .dispatch = remote_proxy_dispatch_internal,
        .destroy = remote_proxy_disconnected
    };
    remote_proxy_t *proxy = calloc(1, sizeof(remote_proxy_t));

    proxy->node_name = strdup(node_name);
    proxy->session_id = strdup(session_id);

    if (safe_str_eq(channel, CRM_SYSTEM_CRMD)) {
        proxy->is_local = TRUE;
    } else {
        proxy->source = mainloop_add_ipc_client(channel, G_PRIORITY_LOW, 0, proxy, &proxy_callbacks);
        proxy->ipc = mainloop_get_ipc_client(proxy->source);

        if (proxy->source == NULL) {
            remote_proxy_free(proxy);
            return NULL;
        }
    }

    crm_trace("created proxy session ID %s", proxy->session_id);
    g_hash_table_insert(proxy_table, proxy->session_id, proxy);

    return proxy;
}

gboolean
crmd_is_proxy_session(const char *session)
{
    return g_hash_table_lookup(proxy_table, session) ? TRUE : FALSE;
}

void
crmd_proxy_send(const char *session, xmlNode *msg)
{
    remote_proxy_t *proxy = g_hash_table_lookup(proxy_table, session);
    lrm_state_t *lrm_state = NULL;

    if (!proxy) {
        return;
    }
    crm_log_xml_trace(msg, "to-proxy");
    lrm_state = lrm_state_find(proxy->node_name);
    if (lrm_state) {
        crm_trace("Sending event to %.8s on %s", proxy->session_id, proxy->node_name);
        remote_proxy_relay_event(lrm_state->conn, session, msg);
    }
}

static void
crmd_proxy_dispatch(const char *session, xmlNode *msg)
{

    crm_log_xml_trace(msg, "CRMd-PROXY[inbound]");

    crm_xml_add(msg, F_CRM_SYS_FROM, session);
    if (crmd_authorize_message(msg, NULL, session)) {
        route_message(C_IPC_MESSAGE, msg);
    }

    trigger_fsa(fsa_source);
}

static void
remote_proxy_cb(lrmd_t *lrmd, void *userdata, xmlNode *msg)
{
    lrm_state_t *lrm_state = userdata;
    const char *op = crm_element_value(msg, F_LRMD_IPC_OP);
    const char *session = crm_element_value(msg, F_LRMD_IPC_SESSION);
    int msg_id = 0;

    /* sessions are raw ipc connections to IPC,
     * all we do is proxy requests/responses exactly
     * like they are given to us at the ipc level. */

    CRM_CHECK(op != NULL, return);
    CRM_CHECK(session != NULL, return);

    crm_element_value_int(msg, F_LRMD_IPC_MSG_ID, &msg_id);

    /* This is msg from remote ipc client going to real ipc server */
    if (safe_str_eq(op, "new")) {
        const char *channel = crm_element_value(msg, F_LRMD_IPC_IPC_SERVER);

        CRM_CHECK(channel != NULL, return);

        if (remote_proxy_new(lrm_state->node_name, session, channel) == NULL) {
            remote_proxy_notify_destroy(lrmd, session);
        }
        crm_trace("new remote proxy client established to %s, session id %s", channel, session);
    } else if (safe_str_eq(op, "destroy")) {
        remote_proxy_end_session(session);

    } else if (safe_str_eq(op, "request")) {
        int flags = 0;
        xmlNode *request = get_message_xml(msg, F_LRMD_IPC_MSG);
        const char *name = crm_element_value(msg, F_LRMD_IPC_CLIENT);
        remote_proxy_t *proxy = g_hash_table_lookup(proxy_table, session);

        CRM_CHECK(request != NULL, return);

        if (proxy == NULL) {
            /* proxy connection no longer exists */
            remote_proxy_notify_destroy(lrmd, session);
            return;
        } else if ((proxy->is_local == FALSE) && (crm_ipc_connected(proxy->ipc) == FALSE)) {
            remote_proxy_end_session(session);
            return;
        }
        proxy->last_request_id = 0;
        crm_element_value_int(msg, F_LRMD_IPC_MSG_FLAGS, &flags);
        crm_xml_add(request, XML_ACL_TAG_ROLE, "pacemaker-remote");

#if ENABLE_ACL
        CRM_ASSERT(lrm_state->node_name);
        crm_acl_get_set_user(request, F_LRMD_IPC_USER, lrm_state->node_name);
#endif

        if (proxy->is_local) {
            /* this is for the crmd, which we are, so don't try
             * and connect/send to ourselves over ipc. instead
             * do it directly. */
            crmd_proxy_dispatch(session, request);
            if (flags & crm_ipc_client_response) {
                xmlNode *op_reply = create_xml_node(NULL, "ack");

                crm_xml_add(op_reply, "function", __FUNCTION__);
                crm_xml_add_int(op_reply, "line", __LINE__);
                remote_proxy_relay_response(lrmd, session, op_reply, msg_id);
                free_xml(op_reply);
            }

        } else if(is_set(flags, crm_ipc_proxied)) {
            const char *type = crm_element_value(request, F_TYPE);
            int rc = 0;

            if (safe_str_eq(type, T_ATTRD)
                && crm_element_value(request, F_ATTRD_HOST) == NULL) {
                crm_xml_add(request, F_ATTRD_HOST, proxy->node_name);
            }

            rc = crm_ipc_send(proxy->ipc, request, flags, 5000, NULL);

            if(rc < 0) {
                xmlNode *op_reply = create_xml_node(NULL, "nack");

                crm_err("Could not relay %s request %d from %s to %s for %s: %s (%d)",
                         op, msg_id, proxy->node_name, crm_ipc_name(proxy->ipc), name, pcmk_strerror(rc), rc);

                /* Send a n'ack so the caller doesn't block */
                crm_xml_add(op_reply, "function", __FUNCTION__);
                crm_xml_add_int(op_reply, "line", __LINE__);
                crm_xml_add_int(op_reply, "rc", rc);
                remote_proxy_relay_response(lrmd, session, op_reply, msg_id);
                free_xml(op_reply);

            } else {
                crm_trace("Relayed %s request %d from %s to %s for %s",
                          op, msg_id, proxy->node_name, crm_ipc_name(proxy->ipc), name);
                proxy->last_request_id = msg_id;
            }

        } else {
            int rc = pcmk_ok;
            xmlNode *op_reply = NULL;
            /* For backwards compatibility with pacemaker_remoted <= 1.1.10 */

            crm_trace("Relaying %s request %d from %s to %s for %s",
                      op, msg_id, proxy->node_name, crm_ipc_name(proxy->ipc), name);

            rc = crm_ipc_send(proxy->ipc, request, flags, 10000, &op_reply);
            if(rc < 0) {
                crm_err("Could not relay %s request %d from %s to %s for %s: %s (%d)",
                         op, msg_id, proxy->node_name, crm_ipc_name(proxy->ipc), name, pcmk_strerror(rc), rc);
            } else {
                crm_trace("Relayed %s request %d from %s to %s for %s",
                          op, msg_id, proxy->node_name, crm_ipc_name(proxy->ipc), name);
            }

            if(op_reply) {
                remote_proxy_relay_response(lrmd, session, op_reply, msg_id);
                free_xml(op_reply);
            }
        }
    } else {
        crm_err("Unknown proxy operation: %s", op);
    }
}

int
lrm_state_remote_connect_async(lrm_state_t * lrm_state, const char *server, int port,
                               int timeout_ms)
{
    int ret;

    if (!lrm_state->conn) {
        lrm_state->conn = lrmd_remote_api_new(lrm_state->node_name, server, port);
        if (!lrm_state->conn) {
            return -1;
        }
        ((lrmd_t *) lrm_state->conn)->cmds->set_callback(lrm_state->conn, remote_lrm_op_callback);
        lrmd_internal_set_proxy_callback(lrm_state->conn, lrm_state, remote_proxy_cb);
    }

    crm_trace("initiating remote connection to %s at %d with timeout %d", server, port, timeout_ms);
    ret =
        ((lrmd_t *) lrm_state->conn)->cmds->connect_async(lrm_state->conn, lrm_state->node_name,
                                                          timeout_ms);

    if (ret != pcmk_ok) {
        lrm_state->num_lrm_register_fails++;
    } else {
        lrm_state->num_lrm_register_fails = 0;
    }

    return ret;
}

int
lrm_state_get_metadata(lrm_state_t * lrm_state,
                       const char *class,
                       const char *provider,
                       const char *agent, char **output, enum lrmd_call_options options)
{
    if (!lrm_state->conn) {
        return -ENOTCONN;
    }

    /* Optimize this... only retrieve metadata from local lrmd connection. Perhaps consider
     * caching result. */
    return ((lrmd_t *) lrm_state->conn)->cmds->get_metadata(lrm_state->conn, class, provider, agent,
                                                            output, options);
}

int
lrm_state_cancel(lrm_state_t * lrm_state, const char *rsc_id, const char *action, int interval)
{
    if (!lrm_state->conn) {
        return -ENOTCONN;
    }

    /* Optimize this, cancel requires a synced request/response to the server.
     * Figure out a way to make this async. */
    if (is_remote_lrmd_ra(NULL, NULL, rsc_id)) {
        return remote_ra_cancel(lrm_state, rsc_id, action, interval);
    }
    return ((lrmd_t *) lrm_state->conn)->cmds->cancel(lrm_state->conn, rsc_id, action, interval);
}

lrmd_rsc_info_t *
lrm_state_get_rsc_info(lrm_state_t * lrm_state, const char *rsc_id, enum lrmd_call_options options)
{
    lrmd_rsc_info_t *rsc = NULL;

    if (!lrm_state->conn) {
        return NULL;
    }
    if (is_remote_lrmd_ra(NULL, NULL, rsc_id)) {
        return remote_ra_get_rsc_info(lrm_state, rsc_id);
    }

    rsc = g_hash_table_lookup(lrm_state->rsc_info_cache, rsc_id);
    if (rsc == NULL) {
        /* only contact the lrmd if we don't already have a cached rsc info */
        rsc = ((lrmd_t *) lrm_state->conn)->cmds->get_rsc_info(lrm_state->conn, rsc_id, options);
        if (rsc == NULL) {
		    return NULL;
        }
        /* cache the result */
        g_hash_table_insert(lrm_state->rsc_info_cache, rsc->id, rsc);
    }

    return lrmd_copy_rsc_info(rsc);

}

int
lrm_state_exec(lrm_state_t * lrm_state, const char *rsc_id, const char *action, const char *userdata, int interval,     /* ms */
               int timeout,     /* ms */
               int start_delay, /* ms */
               lrmd_key_value_t * params)
{

    if (!lrm_state->conn) {
        lrmd_key_value_freeall(params);
        return -ENOTCONN;
    }

    if (is_remote_lrmd_ra(NULL, NULL, rsc_id)) {
        return remote_ra_exec(lrm_state,
                              rsc_id, action, userdata, interval, timeout, start_delay, params);
    }

    return ((lrmd_t *) lrm_state->conn)->cmds->exec(lrm_state->conn,
                                                    rsc_id,
                                                    action,
                                                    userdata,
                                                    interval,
                                                    timeout,
                                                    start_delay,
                                                    lrmd_opt_notify_changes_only, params);
}

int
lrm_state_register_rsc(lrm_state_t * lrm_state,
                       const char *rsc_id,
                       const char *class,
                       const char *provider, const char *agent, enum lrmd_call_options options)
{
    if (!lrm_state->conn) {
        return -ENOTCONN;
    }

    /* optimize this... this function is a synced round trip from client to daemon.
     * The crmd/lrm.c code path should be re-factored to allow the register of resources
     * to be performed async. The lrmd client api needs to make an async version
     * of register available. */
    if (is_remote_lrmd_ra(agent, provider, NULL)) {
        return lrm_state_find_or_create(rsc_id) ? pcmk_ok : -1;
    }

    return ((lrmd_t *) lrm_state->conn)->cmds->register_rsc(lrm_state->conn, rsc_id, class,
                                                            provider, agent, options);
}

int
lrm_state_unregister_rsc(lrm_state_t * lrm_state,
                         const char *rsc_id, enum lrmd_call_options options)
{
    if (!lrm_state->conn) {
        return -ENOTCONN;
    }

    /* optimize this... this function is a synced round trip from client to daemon.
     * The crmd/lrm.c code path that uses this function should always treat it as an
     * async operation. The lrmd client api needs to make an async version unreg available. */
    if (is_remote_lrmd_ra(NULL, NULL, rsc_id)) {
        lrm_state_destroy(rsc_id);
        return pcmk_ok;
    }

    g_hash_table_remove(lrm_state->rsc_info_cache, rsc_id);

    return ((lrmd_t *) lrm_state->conn)->cmds->unregister_rsc(lrm_state->conn, rsc_id, options);
}
