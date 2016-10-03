/* 
 * Copyright (C) 2004 Andrew Beekhof <andrew@beekhof.net>
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

#include <sys/param.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>

#include <crm/crm.h>
#include <crm/cib/internal.h>
#include <crm/msg_xml.h>
#include <crm/common/ipc.h>
#include <crm/common/ipcs.h>
#include <crm/cluster/internal.h>
#include <crm/common/mainloop.h>

#include <crm/common/xml.h>

#include <crm/attrd.h>

#define OPTARGS	"hV"
#if SUPPORT_HEARTBEAT
ll_cluster_t *attrd_cluster_conn;
#endif

GMainLoop *mainloop = NULL;
char *attrd_uname = NULL;
char *attrd_uuid = NULL;
gboolean need_shutdown = FALSE;

GHashTable *attr_hash = NULL;
cib_t *cib_conn = NULL;

typedef struct attr_hash_entry_s {
    char *uuid;
    char *id;
    char *set;
    char *section;

    char *value;
    char *stored_value;

    int timeout;
    char *dampen;
    guint timer_id;

    char *user;

} attr_hash_entry_t;

void attrd_local_callback(xmlNode * msg);
gboolean attrd_timer_callback(void *user_data);
gboolean attrd_trigger_update(attr_hash_entry_t * hash_entry);
void attrd_perform_update(attr_hash_entry_t * hash_entry);

static void
free_hash_entry(gpointer data)
{
    attr_hash_entry_t *entry = data;

    if (entry == NULL) {
        return;
    }
    free(entry->id);
    free(entry->set);
    free(entry->dampen);
    free(entry->section);
    free(entry->uuid);
    free(entry->value);
    free(entry->stored_value);
    free(entry->user);
    free(entry);
}

static int32_t
attrd_ipc_accept(qb_ipcs_connection_t * c, uid_t uid, gid_t gid)
{
    crm_trace("Connection %p", c);
    if (need_shutdown) {
        crm_info("Ignoring new client [%d] during shutdown", crm_ipcs_client_pid(c));
        return -EPERM;
    }

    if (crm_client_new(c, uid, gid) == NULL) {
        return -EIO;
    }
    return 0;
}

static void
attrd_ipc_created(qb_ipcs_connection_t * c)
{
    crm_trace("Connection %p", c);
}

/* Exit code means? */
static int32_t
attrd_ipc_dispatch(qb_ipcs_connection_t * c, void *data, size_t size)
{
    uint32_t id = 0;
    uint32_t flags = 0;
    crm_client_t *client = crm_client_get(c);
    xmlNode *msg = crm_ipcs_recv(client, data, size, &id, &flags);

    crm_ipcs_send_ack(client, id, flags, "ack", __FUNCTION__, __LINE__);
    if (msg == NULL) {
        crm_debug("No msg from %d (%p)", crm_ipcs_client_pid(c), c);
        return 0;
    }
#if ENABLE_ACL
    CRM_ASSERT(client->user != NULL);
    crm_acl_get_set_user(msg, F_ATTRD_USER, client->user);
#endif

    crm_trace("Processing msg from %d (%p)", crm_ipcs_client_pid(c), c);
    crm_log_xml_trace(msg, __FUNCTION__);

    attrd_local_callback(msg);

    free_xml(msg);
    return 0;
}

/* Error code means? */
static int32_t
attrd_ipc_closed(qb_ipcs_connection_t * c)
{
    crm_client_t *client = crm_client_get(c);

    if (client == NULL) {
        return 0;
    }

    crm_trace("Connection %p", c);
    crm_client_destroy(client);
    return 0;
}

static void
attrd_ipc_destroy(qb_ipcs_connection_t * c)
{
    crm_trace("Connection %p", c);
    attrd_ipc_closed(c);
}

struct qb_ipcs_service_handlers ipc_callbacks = {
    .connection_accept = attrd_ipc_accept,
    .connection_created = attrd_ipc_created,
    .msg_process = attrd_ipc_dispatch,
    .connection_closed = attrd_ipc_closed,
    .connection_destroyed = attrd_ipc_destroy
};

static void
attrd_shutdown(int nsig)
{
    need_shutdown = TRUE;
    crm_info("Exiting");
    if (mainloop != NULL && g_main_is_running(mainloop)) {
        g_main_quit(mainloop);
    } else {
        crm_exit(pcmk_ok);
    }
}

static void
usage(const char *cmd, int exit_status)
{
    FILE *stream;

    stream = exit_status ? stderr : stdout;

    fprintf(stream, "usage: %s [-srkh] [-c configure file]\n", cmd);
/* 	fprintf(stream, "\t-d\tsets debug level\n"); */
/* 	fprintf(stream, "\t-s\tgets daemon status\n"); */
/* 	fprintf(stream, "\t-r\trestarts daemon\n"); */
/* 	fprintf(stream, "\t-k\tstops daemon\n"); */
/* 	fprintf(stream, "\t-h\thelp message\n"); */
    fflush(stream);

    crm_exit(exit_status);
}

static void
stop_attrd_timer(attr_hash_entry_t * hash_entry)
{
    if (hash_entry != NULL && hash_entry->timer_id != 0) {
        crm_trace("Stopping %s timer", hash_entry->id);
        g_source_remove(hash_entry->timer_id);
        hash_entry->timer_id = 0;
    }
}

static void
log_hash_entry(int level, attr_hash_entry_t * entry, const char *text)
{
    do_crm_log(level, "%s: Set: %s, Name: %s, Value: %s, Timeout: %s",
               text, entry->section, entry->id, entry->value, entry->dampen);
}

static attr_hash_entry_t *
find_hash_entry(xmlNode * msg)
{
    const char *value = NULL;
    const char *attr = crm_element_value(msg, F_ATTRD_ATTRIBUTE);
    attr_hash_entry_t *hash_entry = NULL;

    if (attr == NULL) {
        crm_info("Ignoring message with no attribute name");
        return NULL;
    }

    hash_entry = g_hash_table_lookup(attr_hash, attr);

    if (hash_entry == NULL) {
        /* create one and add it */
        crm_info("Creating hash entry for %s", attr);
        hash_entry = calloc(1, sizeof(attr_hash_entry_t));
        hash_entry->id = strdup(attr);

        g_hash_table_insert(attr_hash, hash_entry->id, hash_entry);
        hash_entry = g_hash_table_lookup(attr_hash, attr);
        CRM_CHECK(hash_entry != NULL, return NULL);
    }

    value = crm_element_value(msg, F_ATTRD_SET);
    if (value != NULL) {
        free(hash_entry->set);
        hash_entry->set = strdup(value);
        crm_debug("\t%s->set: %s", attr, value);
    }

    value = crm_element_value(msg, F_ATTRD_SECTION);
    if (value == NULL) {
        value = XML_CIB_TAG_STATUS;
    }
    free(hash_entry->section);
    hash_entry->section = strdup(value);
    crm_trace("\t%s->section: %s", attr, value);

    value = crm_element_value(msg, F_ATTRD_DAMPEN);
    if (value != NULL) {
        free(hash_entry->dampen);
        hash_entry->dampen = strdup(value);

        hash_entry->timeout = crm_get_msec(value);
        crm_trace("\t%s->timeout: %s", attr, value);
    }
#if ENABLE_ACL
    free(hash_entry->user);
    hash_entry->user = NULL;

    value = crm_element_value(msg, F_ATTRD_USER);
    if (value != NULL) {
        hash_entry->user = strdup(value);
        crm_trace("\t%s->user: %s", attr, value);
    }
#endif

    log_hash_entry(LOG_DEBUG_2, hash_entry, "Found (and updated) entry:");
    return hash_entry;
}

#if SUPPORT_HEARTBEAT
static void
attrd_ha_connection_destroy(gpointer user_data)
{
    crm_trace("Invoked");
    if (need_shutdown) {
        /* we signed out, so this is expected */
        crm_info("Heartbeat disconnection complete");
        return;
    }

    crm_crit("Lost connection to heartbeat service!");
    if (mainloop != NULL && g_main_is_running(mainloop)) {
        g_main_quit(mainloop);
        return;
    }
    crm_exit(pcmk_ok);
}

static void
attrd_ha_callback(HA_Message * msg, void *private_data)
{
    attr_hash_entry_t *hash_entry = NULL;
    xmlNode *xml = convert_ha_message(NULL, msg, __FUNCTION__);
    const char *from = crm_element_value(xml, F_ORIG);
    const char *op = crm_element_value(xml, F_ATTRD_TASK);
    const char *host = crm_element_value(xml, F_ATTRD_HOST);
    const char *ignore = crm_element_value(xml, F_ATTRD_IGNORE_LOCALLY);

    if (host != NULL && safe_str_eq(host, attrd_uname)) {
        crm_info("Update relayed from %s", from);
        attrd_local_callback(xml);

    } else if (ignore == NULL || safe_str_neq(from, attrd_uname)) {
        crm_info("%s message from %s", op, from);
        hash_entry = find_hash_entry(xml);
        stop_attrd_timer(hash_entry);
        attrd_perform_update(hash_entry);
    }
    free_xml(xml);
}

#endif

#if SUPPORT_COROSYNC
static void
attrd_cs_dispatch(cpg_handle_t handle,
                 const struct cpg_name *groupName,
                 uint32_t nodeid, uint32_t pid, void *msg, size_t msg_len)
{
    uint32_t kind = 0;
    xmlNode *xml = NULL;
    const char *from = NULL;
    char *data = pcmk_message_common_cs(handle, nodeid, pid, msg, &kind, &from);

    if(data == NULL) {
        return;
    }
    if (kind == crm_class_cluster) {
        xml = string2xml(data);
        if (xml == NULL) {
            crm_err("Bad message received: '%.120s'", data);
        }
    }

    if (xml != NULL) {
        attr_hash_entry_t *hash_entry = NULL;
        const char *op = crm_element_value(xml, F_ATTRD_TASK);
        const char *host = crm_element_value(xml, F_ATTRD_HOST);
        const char *ignore = crm_element_value(xml, F_ATTRD_IGNORE_LOCALLY);

        /* crm_xml_add_int(xml, F_SEQ, wrapper->id); */
        crm_xml_add(xml, F_ORIG, from);

        if (host != NULL && safe_str_eq(host, attrd_uname)) {
            crm_notice("Update relayed from %s", from);
            attrd_local_callback(xml);

        } else if (ignore == NULL || safe_str_neq(from, attrd_uname)) {
            crm_trace("%s message from %s", op, from);
            hash_entry = find_hash_entry(xml);
            stop_attrd_timer(hash_entry);
            attrd_perform_update(hash_entry);
        }

        free_xml(xml);
    }

    free(data);
}

static void
attrd_cs_destroy(gpointer unused)
{
    if (need_shutdown) {
        /* we signed out, so this is expected */
        crm_info("Corosync disconnection complete");
        return;
    }

    crm_crit("Lost connection to Corosync service!");
    if (mainloop != NULL && g_main_is_running(mainloop)) {
        g_main_quit(mainloop);
        return;
    }
    crm_exit(EINVAL);
}
#endif

static void
attrd_cib_connection_destroy(gpointer user_data)
{
    cib_t *conn = user_data;

    conn->cmds->signoff(conn);  /* Ensure IPC is cleaned up */

    if (need_shutdown) {
        crm_info("Connection to the CIB terminated...");

    } else {
        /* eventually this will trigger a reconnect, not a shutdown */
        crm_err("Connection to the CIB terminated...");
        crm_exit(ENOTCONN);
    }

    return;
}

static void
update_for_hash_entry(gpointer key, gpointer value, gpointer user_data)
{
    attr_hash_entry_t *entry = value;

    if (entry->value != NULL || entry->stored_value != NULL) {
        attrd_timer_callback(value);
    }
}

static void
local_update_for_hash_entry(gpointer key, gpointer value, gpointer user_data)
{
    attr_hash_entry_t *entry = value;

    if (entry->timer_id == 0) {
        crm_trace("Performing local-only update after replace for %s", entry->id);
        attrd_perform_update(entry);
        /* } else {
         *     just let the timer expire and attrd_timer_callback() will do the right thing
         */
    }
}

static void
do_cib_replaced(const char *event, xmlNode * msg)
{
    crm_info("Updating all attributes after %s event", event);
    g_hash_table_foreach(attr_hash, local_update_for_hash_entry, NULL);
}

static gboolean
cib_connect(void *user_data)
{
    static int attempts = 1;
    static int max_retry = 20;
    gboolean was_err = FALSE;
    static cib_t *local_conn = NULL;

    if (local_conn == NULL) {
        local_conn = cib_new();
    }

    if (was_err == FALSE) {
        int rc = -ENOTCONN;

        if (attempts < max_retry) {
            crm_debug("CIB signon attempt %d", attempts);
            rc = local_conn->cmds->signon(local_conn, T_ATTRD, cib_command);
        }

        if (rc != pcmk_ok && attempts > max_retry) {
            crm_err("Signon to CIB failed: %s", pcmk_strerror(rc));
            was_err = TRUE;

        } else if (rc != pcmk_ok) {
            attempts++;
            return TRUE;
        }
    }

    crm_info("Connected to the CIB after %d signon attempts", attempts);

    if (was_err == FALSE) {
        int rc = local_conn->cmds->set_connection_dnotify(local_conn, attrd_cib_connection_destroy);

        if (rc != pcmk_ok) {
            crm_err("Could not set dnotify callback");
            was_err = TRUE;
        }
    }

    if (was_err == FALSE) {
        if (pcmk_ok !=
            local_conn->cmds->add_notify_callback(local_conn, T_CIB_REPLACE_NOTIFY,
                                                  do_cib_replaced)) {
            crm_err("Could not set CIB notification callback");
            was_err = TRUE;
        }
    }

    if (was_err) {
        crm_err("Aborting startup");
        crm_exit(DAEMON_RESPAWN_STOP);
    }

    cib_conn = local_conn;

    crm_info("Sending full refresh now that we're connected to the cib");
    g_hash_table_foreach(attr_hash, local_update_for_hash_entry, NULL);

    return FALSE;
}

int
main(int argc, char **argv)
{
    int flag = 0;
    int argerr = 0;
    crm_cluster_t cluster;
    gboolean was_err = FALSE;
    qb_ipcs_connection_t *c = NULL;
    qb_ipcs_service_t *ipcs = NULL;

    crm_log_init(T_ATTRD, LOG_NOTICE, TRUE, FALSE, argc, argv, FALSE);
    mainloop_add_signal(SIGTERM, attrd_shutdown);

    while ((flag = getopt(argc, argv, OPTARGS)) != EOF) {
        switch (flag) {
            case 'V':
                crm_bump_log_level(argc, argv);
                break;
            case 'h':          /* Help message */
                usage(T_ATTRD, EX_OK);
                break;
            default:
                ++argerr;
                break;
        }
    }

    if (optind > argc) {
        ++argerr;
    }

    if (argerr) {
        usage(T_ATTRD, EX_USAGE);
    }

    attr_hash = g_hash_table_new_full(crm_str_hash, g_str_equal, NULL, free_hash_entry);

    crm_info("Starting up");

    if (was_err == FALSE) {

#if SUPPORT_COROSYNC
        if (is_openais_cluster()) {
            cluster.destroy = attrd_cs_destroy;
            cluster.cpg.cpg_deliver_fn = attrd_cs_dispatch;
            cluster.cpg.cpg_confchg_fn = pcmk_cpg_membership;
        }
#endif

#if SUPPORT_HEARTBEAT
        if (is_heartbeat_cluster()) {
            cluster.hb_conn = NULL;
            cluster.hb_dispatch = attrd_ha_callback;
            cluster.destroy = attrd_ha_connection_destroy;
        }
#endif

        if (FALSE == crm_cluster_connect(&cluster)) {
            crm_err("HA Signon failed");
            was_err = TRUE;
        }

        attrd_uname = cluster.uname;
        attrd_uuid = cluster.uuid;
#if SUPPORT_HEARTBEAT
        attrd_cluster_conn = cluster.hb_conn;
#endif
    }

    crm_info("Cluster connection active");

    if (was_err == FALSE) {
        attrd_ipc_server_init(&ipcs, &ipc_callbacks);
    }

    crm_info("Accepting attribute updates");

    mainloop = g_main_new(FALSE);

    if (0 == g_timeout_add_full(G_PRIORITY_LOW + 1, 5000, cib_connect, NULL, NULL)) {
        crm_info("Adding timer failed");
        was_err = TRUE;
    }

    if (was_err) {
        crm_err("Aborting startup");
        return 100;
    }

    crm_notice("Starting mainloop...");
    g_main_run(mainloop);
    crm_notice("Exiting...");

#if SUPPORT_HEARTBEAT
    if (is_heartbeat_cluster()) {
        attrd_cluster_conn->llc_ops->signoff(attrd_cluster_conn, TRUE);
        attrd_cluster_conn->llc_ops->delete(attrd_cluster_conn);
    }
#endif

    c = qb_ipcs_connection_first_get(ipcs);
    while (c != NULL) {
        qb_ipcs_connection_t *last = c;

        c = qb_ipcs_connection_next_get(ipcs, last);

        /* There really shouldn't be anyone connected at this point */
        crm_notice("Disconnecting client %p, pid=%d...", last, crm_ipcs_client_pid(last));
        qb_ipcs_disconnect(last);
        qb_ipcs_connection_unref(last);
    }

    qb_ipcs_destroy(ipcs);

    if (cib_conn) {
        cib_conn->cmds->signoff(cib_conn);
        cib_delete(cib_conn);
    }

    g_hash_table_destroy(attr_hash);
    free(attrd_uuid);

    return crm_exit(pcmk_ok);
}

struct attrd_callback_s {
    char *attr;
    char *value;
};

/*
 * \internal
 * \brief Free an attrd callback structure
 */
static void
free_attrd_callback(void *user_data)
{
    struct attrd_callback_s *data = user_data;

    free(data->attr);
    free(data->value);
    free(data);
}

static void
attrd_cib_callback(xmlNode * msg, int call_id, int rc, xmlNode * output, void *user_data)
{
    attr_hash_entry_t *hash_entry = NULL;
    struct attrd_callback_s *data = user_data;

    if (data->value == NULL && rc == -ENXIO) {
        rc = pcmk_ok;

    } else if (call_id < 0) {
        crm_warn("Update %s=%s failed: %s", data->attr, data->value, pcmk_strerror(call_id));
        return;
    }

    switch (rc) {
        case pcmk_ok:
            crm_debug("Update %d for %s=%s passed", call_id, data->attr, data->value);
            hash_entry = g_hash_table_lookup(attr_hash, data->attr);

            if (hash_entry) {
                free(hash_entry->stored_value);
                hash_entry->stored_value = NULL;
                if (data->value != NULL) {
                    hash_entry->stored_value = strdup(data->value);
                }
            }
            break;
        case -pcmk_err_diff_failed:    /* When an attr changes while the CIB is syncing */
        case -ETIME:           /* When an attr changes while there is a DC election */
        case -ENXIO:           /* When an attr changes while the CIB is syncing a
                                 *   newer config from a node that just came up
                                 */
            crm_warn("Update %d for %s=%s failed: %s",
                     call_id, data->attr, data->value, pcmk_strerror(rc));
            break;
        default:
            crm_err("Update %d for %s=%s failed: %s",
                    call_id, data->attr, data->value, pcmk_strerror(rc));
    }
}

void
attrd_perform_update(attr_hash_entry_t * hash_entry)
{
    int rc = pcmk_ok;
    struct attrd_callback_s *data = NULL;
    const char *user_name = NULL;

    if (hash_entry == NULL) {
        return;

    } else if (cib_conn == NULL) {
        crm_info("Delaying operation %s=%s: cib not connected", hash_entry->id,
                 crm_str(hash_entry->value));
        return;

    }
#if ENABLE_ACL
    if (hash_entry->user) {
        user_name = hash_entry->user;
        crm_trace("Performing request from user '%s'", hash_entry->user);
    }
#endif

    if (hash_entry->value == NULL) {
        /* delete the attr */
        rc = delete_attr_delegate(cib_conn, cib_none, hash_entry->section, attrd_uuid, NULL,
                                  hash_entry->set, hash_entry->uuid, hash_entry->id, NULL, FALSE,
                                  user_name);

        if (rc >= 0 && hash_entry->stored_value) {
            crm_notice("Sent delete %d: node=%s, attr=%s, id=%s, set=%s, section=%s",
                       rc, attrd_uuid, hash_entry->id,
                       hash_entry->uuid ? hash_entry->uuid : "<n/a>", hash_entry->set,
                       hash_entry->section);

        } else if (rc < 0 && rc != -ENXIO) {
            crm_notice
                ("Delete operation failed: node=%s, attr=%s, id=%s, set=%s, section=%s: %s (%d)",
                 attrd_uuid, hash_entry->id, hash_entry->uuid ? hash_entry->uuid : "<n/a>",
                 hash_entry->set, hash_entry->section, pcmk_strerror(rc), rc);

        } else {
            crm_trace("Sent delete %d: node=%s, attr=%s, id=%s, set=%s, section=%s",
                      rc, attrd_uuid, hash_entry->id,
                      hash_entry->uuid ? hash_entry->uuid : "<n/a>", hash_entry->set,
                      hash_entry->section);
        }

    } else {
        /* send update */
        rc = update_attr_delegate(cib_conn, cib_none, hash_entry->section,
                                  attrd_uuid, NULL, hash_entry->set, hash_entry->uuid,
                                  hash_entry->id, hash_entry->value, FALSE, user_name, NULL);
        if (rc < 0) {
            crm_notice("Sent update %s=%s failed: %s", hash_entry->id, hash_entry->value,
                       pcmk_strerror(rc));
        }
        if (safe_str_neq(hash_entry->value, hash_entry->stored_value) || rc < 0) {
            crm_notice("Sent update %d: %s=%s", rc, hash_entry->id, hash_entry->value);
        } else {
            crm_trace("Sent update %d: %s=%s", rc, hash_entry->id, hash_entry->value);
        }
    }

    data = calloc(1, sizeof(struct attrd_callback_s));
    data->attr = strdup(hash_entry->id);
    if (hash_entry->value != NULL) {
        data->value = strdup(hash_entry->value);
    }
    cib_conn->cmds->register_callback_full(cib_conn, rc, 120, FALSE, data,
                                           "attrd_cib_callback",
                                           attrd_cib_callback,
                                           free_attrd_callback);
    return;
}

void
attrd_local_callback(xmlNode * msg)
{
    static int plus_plus_len = 5;
    attr_hash_entry_t *hash_entry = NULL;
    const char *from = crm_element_value(msg, F_ORIG);
    const char *op = crm_element_value(msg, F_ATTRD_TASK);
    const char *attr = crm_element_value(msg, F_ATTRD_ATTRIBUTE);
    const char *value = crm_element_value(msg, F_ATTRD_VALUE);
    const char *host = crm_element_value(msg, F_ATTRD_HOST);

    if (safe_str_eq(op, "refresh")) {
        crm_notice("Sending full refresh (origin=%s)", from);
        g_hash_table_foreach(attr_hash, update_for_hash_entry, NULL);
        return;
    } else if (safe_str_eq(op, ATTRD_OP_PEER_REMOVE)) {
        /* The legacy code didn't understand this command - swallow silently */
        return;
    }

    if (host != NULL && safe_str_neq(host, attrd_uname)) {
        send_cluster_message(crm_get_peer(0, host), crm_msg_attrd, msg, FALSE);
        return;
    }

    crm_debug("%s message from %s: %s=%s", op, from, attr, crm_str(value));
    hash_entry = find_hash_entry(msg);
    if (hash_entry == NULL) {
        return;
    }

    if (hash_entry->uuid == NULL) {
        const char *key = crm_element_value(msg, F_ATTRD_KEY);

        if (key) {
            hash_entry->uuid = strdup(key);
        }
    }

    crm_debug("Supplied: %s, Current: %s, Stored: %s",
              value, hash_entry->value, hash_entry->stored_value);

    if (safe_str_eq(value, hash_entry->value)
        && safe_str_eq(value, hash_entry->stored_value)) {
        crm_trace("Ignoring non-change");
        return;

    } else if (value) {
        int offset = 1;
        int int_value = 0;
        int value_len = strlen(value);

        if (value_len < (plus_plus_len + 2)
            || value[plus_plus_len] != '+'
            || (value[plus_plus_len + 1] != '+' && value[plus_plus_len + 1] != '=')) {
            goto set_unexpanded;
        }

        int_value = char2score(hash_entry->value);
        if (value[plus_plus_len + 1] != '+') {
            const char *offset_s = value + (plus_plus_len + 2);

            offset = char2score(offset_s);
        }
        int_value += offset;

        if (int_value > INFINITY) {
            int_value = INFINITY;
        }

        crm_info("Expanded %s=%s to %d", attr, value, int_value);
        crm_xml_add_int(msg, F_ATTRD_VALUE, int_value);
        value = crm_element_value(msg, F_ATTRD_VALUE);
    }

  set_unexpanded:
    if (safe_str_eq(value, hash_entry->value) && hash_entry->timer_id) {
        /* We're already waiting to set this value */
        return;
    }

    free(hash_entry->value);
    hash_entry->value = NULL;
    if (value != NULL) {
        hash_entry->value = strdup(value);
        crm_debug("New value of %s is %s", attr, value);
    }

    stop_attrd_timer(hash_entry);

    if (hash_entry->timeout > 0) {
        hash_entry->timer_id = g_timeout_add(hash_entry->timeout, attrd_timer_callback, hash_entry);
    } else {
        attrd_trigger_update(hash_entry);
    }

    return;
}

gboolean
attrd_timer_callback(void *user_data)
{
    stop_attrd_timer(user_data);
    attrd_trigger_update(user_data);
    return TRUE;                /* Always return true, removed cleanly by stop_attrd_timer() */
}

gboolean
attrd_trigger_update(attr_hash_entry_t * hash_entry)
{
    xmlNode *msg = NULL;

    /* send HA message to everyone */
    crm_notice("Sending flush op to all hosts for: %s (%s)",
               hash_entry->id, crm_str(hash_entry->value));
    log_hash_entry(LOG_DEBUG_2, hash_entry, "Sending flush op to all hosts for:");

    msg = create_xml_node(NULL, __FUNCTION__);
    crm_xml_add(msg, F_TYPE, T_ATTRD);
    crm_xml_add(msg, F_ORIG, attrd_uname);
    crm_xml_add(msg, F_ATTRD_TASK, "flush");
    crm_xml_add(msg, F_ATTRD_ATTRIBUTE, hash_entry->id);
    crm_xml_add(msg, F_ATTRD_SET, hash_entry->set);
    crm_xml_add(msg, F_ATTRD_SECTION, hash_entry->section);
    crm_xml_add(msg, F_ATTRD_DAMPEN, hash_entry->dampen);
    crm_xml_add(msg, F_ATTRD_VALUE, hash_entry->value);
#if ENABLE_ACL
    if (hash_entry->user) {
        crm_xml_add(msg, F_ATTRD_USER, hash_entry->user);
    }
#endif

    if (hash_entry->timeout <= 0) {
        crm_xml_add(msg, F_ATTRD_IGNORE_LOCALLY, hash_entry->value);
        attrd_perform_update(hash_entry);
    }

    send_cluster_message(NULL, crm_msg_attrd, msg, FALSE);
    free_xml(msg);

    return TRUE;
}
