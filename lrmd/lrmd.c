/*
 * Copyright (c) 2012 David Vossel <davidvossel@gmail.com>
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
 */

#include <crm_internal.h>

#include <glib.h>
#include <unistd.h>

#include <crm/crm.h>
#include <crm/services.h>
#include <crm/common/mainloop.h>
#include <crm/common/ipc.h>
#include <crm/common/ipcs.h>
#include <crm/msg_xml.h>

#include <lrmd_private.h>

#ifdef HAVE_SYS_TIMEB_H
#  include <sys/timeb.h>
#endif

#define EXIT_REASON_MAX_LEN 128

GHashTable *rsc_list = NULL;

typedef struct lrmd_cmd_s {
    int timeout;
    int interval;
    int start_delay;
    int timeout_orig;

    int call_id;
    int exec_rc;
    int lrmd_op_status;

    int call_opts;
    /* Timer ids, must be removed on cmd destruction. */
    int delay_id;
    int stonith_recurring_id;

    int rsc_deleted;

    int service_flags;

    char *client_id;
    char *origin;
    char *rsc_id;
    char *action;
    char *real_action;
    char *exit_reason;
    char *output;
    char *userdata_str;

    /* when set, this cmd should go through a container wrapper */
    const char *isolation_wrapper;

#ifdef HAVE_SYS_TIMEB_H
    /* recurring and systemd operations may involve more than one lrmd command
     * per operation, so they need info about original and most recent
     */
    struct timeb t_first_run;   /* Timestamp of when op first ran */
    struct timeb t_run;         /* Timestamp of when op most recently ran */
    struct timeb t_first_queue; /* Timestamp of when op first was queued */
    struct timeb t_queue;       /* Timestamp of when op most recently was queued */
    struct timeb t_rcchange;    /* Timestamp of last rc change */
#endif

    int first_notify_sent;
    int last_notify_rc;
    int last_notify_op_status;
    int last_pid;

    GHashTable *params;
} lrmd_cmd_t;

static void cmd_finalize(lrmd_cmd_t * cmd, lrmd_rsc_t * rsc);
static gboolean lrmd_rsc_dispatch(gpointer user_data);
static void cancel_all_recurring(lrmd_rsc_t * rsc, const char *client_id);

static void
log_finished(lrmd_cmd_t * cmd, int exec_time, int queue_time)
{
    char pid_str[32] = { 0, };
    int log_level = LOG_INFO;

    if (cmd->last_pid) {
        snprintf(pid_str, 32, "%d", cmd->last_pid);
    }

    if (safe_str_eq(cmd->action, "monitor")) {
        log_level = LOG_DEBUG;
    }
#ifdef HAVE_SYS_TIMEB_H
    do_crm_log(log_level,
               "finished - rsc:%s action:%s call_id:%d %s%s exit-code:%d exec-time:%dms queue-time:%dms",
               cmd->rsc_id, cmd->action, cmd->call_id, cmd->last_pid ? "pid:" : "", pid_str,
               cmd->exec_rc, exec_time, queue_time);
#else
    do_crm_log(log_level, "finished - rsc:%s action:%s call_id:%d %s%s exit-code:%d",
               cmd->rsc_id,
               cmd->action, cmd->call_id, cmd->last_pid ? "pid:" : "", pid_str, cmd->exec_rc);
#endif
}

static void
log_execute(lrmd_cmd_t * cmd)
{
    int log_level = LOG_INFO;

    if (safe_str_eq(cmd->action, "monitor")) {
        log_level = LOG_DEBUG;
    }

    do_crm_log(log_level, "executing - rsc:%s action:%s call_id:%d",
               cmd->rsc_id, cmd->action, cmd->call_id);
}

static const char *
normalize_action_name(lrmd_rsc_t * rsc, const char *action)
{
    if (safe_str_eq(action, "monitor") &&
        (safe_str_eq(rsc->class, "lsb") ||
         safe_str_eq(rsc->class, "service") || safe_str_eq(rsc->class, "systemd"))) {
        return "status";
    }
    return action;
}

static lrmd_rsc_t *
build_rsc_from_xml(xmlNode * msg)
{
    xmlNode *rsc_xml = get_xpath_object("//" F_LRMD_RSC, msg, LOG_ERR);
    lrmd_rsc_t *rsc = NULL;

    rsc = calloc(1, sizeof(lrmd_rsc_t));

    crm_element_value_int(msg, F_LRMD_CALLOPTS, &rsc->call_opts);

    rsc->rsc_id = crm_element_value_copy(rsc_xml, F_LRMD_RSC_ID);
    rsc->class = crm_element_value_copy(rsc_xml, F_LRMD_CLASS);
    rsc->provider = crm_element_value_copy(rsc_xml, F_LRMD_PROVIDER);
    rsc->type = crm_element_value_copy(rsc_xml, F_LRMD_TYPE);
    rsc->work = mainloop_add_trigger(G_PRIORITY_HIGH, lrmd_rsc_dispatch, rsc);
    return rsc;
}

static lrmd_cmd_t *
create_lrmd_cmd(xmlNode * msg, crm_client_t * client, lrmd_rsc_t *rsc)
{
    int call_options = 0;
    xmlNode *rsc_xml = get_xpath_object("//" F_LRMD_RSC, msg, LOG_ERR);
    lrmd_cmd_t *cmd = NULL;

    cmd = calloc(1, sizeof(lrmd_cmd_t));

    crm_element_value_int(msg, F_LRMD_CALLOPTS, &call_options);
    cmd->call_opts = call_options;
    cmd->client_id = strdup(client->id);

    crm_element_value_int(msg, F_LRMD_CALLID, &cmd->call_id);
    crm_element_value_int(rsc_xml, F_LRMD_RSC_INTERVAL, &cmd->interval);
    crm_element_value_int(rsc_xml, F_LRMD_TIMEOUT, &cmd->timeout);
    crm_element_value_int(rsc_xml, F_LRMD_RSC_START_DELAY, &cmd->start_delay);
    cmd->timeout_orig = cmd->timeout;

    cmd->origin = crm_element_value_copy(rsc_xml, F_LRMD_ORIGIN);
    cmd->action = crm_element_value_copy(rsc_xml, F_LRMD_RSC_ACTION);
    cmd->userdata_str = crm_element_value_copy(rsc_xml, F_LRMD_RSC_USERDATA_STR);
    cmd->rsc_id = crm_element_value_copy(rsc_xml, F_LRMD_RSC_ID);

    cmd->params = xml2list(rsc_xml);
    cmd->isolation_wrapper = g_hash_table_lookup(cmd->params, "CRM_meta_isolation_wrapper");

    if (cmd->isolation_wrapper) {
        if (g_hash_table_lookup(cmd->params, "CRM_meta_isolation_instance") == NULL) {
            g_hash_table_insert(cmd->params, strdup("CRM_meta_isolation_instance"), strdup(rsc->rsc_id));
        }
        if (rsc->provider) {
            g_hash_table_insert(cmd->params, strdup("CRM_meta_provider"), strdup(rsc->provider));
        }
        g_hash_table_insert(cmd->params, strdup("CRM_meta_class"), strdup(rsc->class));
        g_hash_table_insert(cmd->params, strdup("CRM_meta_type"), strdup(rsc->type));
    }

    if (safe_str_eq(g_hash_table_lookup(cmd->params, "CRM_meta_on_fail"), "block")) {
        crm_debug("Setting flag to leave pid group on timeout and only kill action pid for %s_%s_%d", cmd->rsc_id, cmd->action, cmd->interval);
        cmd->service_flags |= SVC_ACTION_LEAVE_GROUP;
    }
    return cmd;
}

static void
free_lrmd_cmd(lrmd_cmd_t * cmd)
{
    if (cmd->stonith_recurring_id) {
        g_source_remove(cmd->stonith_recurring_id);
    }
    if (cmd->delay_id) {
        g_source_remove(cmd->delay_id);
    }
    if (cmd->params) {
        g_hash_table_destroy(cmd->params);
    }
    free(cmd->origin);
    free(cmd->action);
    free(cmd->real_action);
    free(cmd->userdata_str);
    free(cmd->rsc_id);
    free(cmd->output);
    free(cmd->exit_reason);
    free(cmd->client_id);
    free(cmd);
}

static gboolean
stonith_recurring_op_helper(gpointer data)
{
    lrmd_cmd_t *cmd = data;
    lrmd_rsc_t *rsc;

    cmd->stonith_recurring_id = 0;

    if (!cmd->rsc_id) {
        return FALSE;
    }

    rsc = g_hash_table_lookup(rsc_list, cmd->rsc_id);

    CRM_ASSERT(rsc != NULL);
    /* take it out of recurring_ops list, and put it in the pending ops
     * to be executed */
    rsc->recurring_ops = g_list_remove(rsc->recurring_ops, cmd);
    rsc->pending_ops = g_list_append(rsc->pending_ops, cmd);
#ifdef HAVE_SYS_TIMEB_H
    ftime(&cmd->t_queue);
    if (cmd->t_first_queue.time == 0) {
        cmd->t_first_queue = cmd->t_queue;
    }
#endif
    mainloop_set_trigger(rsc->work);

    return FALSE;
}

static gboolean
start_delay_helper(gpointer data)
{
    lrmd_cmd_t *cmd = data;
    lrmd_rsc_t *rsc = NULL;

    cmd->delay_id = 0;
    rsc = cmd->rsc_id ? g_hash_table_lookup(rsc_list, cmd->rsc_id) : NULL;

    if (rsc) {
        mainloop_set_trigger(rsc->work);
    }

    return FALSE;
}

static gboolean
merge_recurring_duplicate(lrmd_rsc_t * rsc, lrmd_cmd_t * cmd)
{
    GListPtr gIter = NULL;
    lrmd_cmd_t * dup = NULL;
    gboolean dup_pending = FALSE;

    if (cmd->interval == 0) {
        return 0;
    }

    for (gIter = rsc->pending_ops; gIter != NULL; gIter = gIter->next) {
        dup = gIter->data;
        if (safe_str_eq(cmd->action, dup->action) && cmd->interval == dup->interval) {
            dup_pending = TRUE;
            goto merge_dup;
        }
    }

    /* if dup is in recurring_ops list, that means it has already executed
     * and is in the interval loop. we can't just remove it in this case. */
    for (gIter = rsc->recurring_ops; gIter != NULL; gIter = gIter->next) {
        dup = gIter->data;
        if (safe_str_eq(cmd->action, dup->action) && cmd->interval == dup->interval) {
            goto merge_dup;
        }
    }

    return FALSE;
merge_dup:


    /* This should not occur, if it does we need to investigate in the crmd
     * how something like this is possible */
    crm_warn("Duplicate recurring op entry detected (%s_%s_%d), merging with previous op entry",
            rsc->rsc_id,
            normalize_action_name(rsc, dup->action),
            dup->interval);

    /* merge */
    dup->first_notify_sent = 0;
    free(dup->userdata_str);
    dup->userdata_str = cmd->userdata_str;
    cmd->userdata_str = NULL;
    dup->call_id = cmd->call_id;

    if (safe_str_eq(rsc->class, "stonith")) {
        /* if we are waiting for the next interval, kick it off now */
        if (dup_pending == TRUE) {
            g_source_remove(cmd->stonith_recurring_id);
            cmd->stonith_recurring_id = 0;
            stonith_recurring_op_helper(cmd);
        }

    } else if (dup_pending == FALSE) {
        /* if we've already handed this to the service lib, kick off an early execution */
        services_action_kick(rsc->rsc_id, normalize_action_name(rsc, dup->action), dup->interval);
    }
    free_lrmd_cmd(cmd);

    return TRUE;
}

static void
schedule_lrmd_cmd(lrmd_rsc_t * rsc, lrmd_cmd_t * cmd)
{
    gboolean dup_processed = FALSE;
    CRM_CHECK(cmd != NULL, return);
    CRM_CHECK(rsc != NULL, return);

    crm_trace("Scheduling %s on %s", cmd->action, rsc->rsc_id);

    dup_processed = merge_recurring_duplicate(rsc, cmd);
    if (dup_processed) {
        /* duplicate recurring cmd found, cmds merged */
        return;
    }

    /* crmd expects lrmd to automatically cancel recurring ops before rsc stops. */
    if (rsc && safe_str_eq(cmd->action, "stop")) {
        cancel_all_recurring(rsc, NULL);
    }

    rsc->pending_ops = g_list_append(rsc->pending_ops, cmd);
#ifdef HAVE_SYS_TIMEB_H
    ftime(&cmd->t_queue);
    if (cmd->t_first_queue.time == 0) {
        cmd->t_first_queue = cmd->t_queue;
    }
#endif
    mainloop_set_trigger(rsc->work);

    if (cmd->start_delay) {
        cmd->delay_id = g_timeout_add(cmd->start_delay, start_delay_helper, cmd);
    }
}

static void
send_reply(crm_client_t * client, int rc, uint32_t id, int call_id)
{
    int send_rc = 0;
    xmlNode *reply = NULL;

    reply = create_xml_node(NULL, T_LRMD_REPLY);
    crm_xml_add(reply, F_LRMD_ORIGIN, __FUNCTION__);
    crm_xml_add_int(reply, F_LRMD_RC, rc);
    crm_xml_add_int(reply, F_LRMD_CALLID, call_id);

    send_rc = lrmd_server_send_reply(client, id, reply);

    free_xml(reply);
    if (send_rc < 0) {
        crm_warn("LRMD reply to %s failed: %d", client->name, send_rc);
    }
}

static void
send_client_notify(gpointer key, gpointer value, gpointer user_data)
{
    xmlNode *update_msg = user_data;
    crm_client_t *client = value;

    if (client == NULL) {
        crm_err("Asked to send event to  NULL client");
        return;
    } else if (client->name == NULL) {
        crm_trace("Asked to send event to client with no name");
        return;
    }

    if (lrmd_server_send_notify(client, update_msg) <= 0) {
        crm_warn("Notification of client %s/%s failed", client->name, client->id);
    }
}

#ifdef HAVE_SYS_TIMEB_H
/*!
 * \internal
 * \brief Return difference between two times in milliseconds
 *
 * \param[in] now  More recent time (or NULL to use current time)
 * \param[in] old  Earlier time
 *
 * \return milliseconds difference (or 0 if old is NULL or has time zero)
 */
static int
time_diff_ms(struct timeb *now, struct timeb *old)
{
    struct timeb local_now = { 0, };

    if (now == NULL) {
        ftime(&local_now);
        now = &local_now;
    }
    if ((old == NULL) || (old->time == 0)) {
        return 0;
    }
    return difftime(now->time, old->time) * 1000 + now->millitm - old->millitm;
}

/*!
 * \internal
 * \brief Reset a command's operation times to their original values.
 *
 * Reset a command's run and queued timestamps to the timestamps of the original
 * command, so we report the entire time since then and not just the time since
 * the most recent command (for recurring and systemd operations).
 *
 * /param[in] cmd  LRMD command object to reset
 *
 * /note It's not obvious what the queued time should be for a systemd
 * start/stop operation, which might go like this:
 *   initial command queued 5ms, runs 3s
 *   monitor command queued 10ms, runs 10s
 *   monitor command queued 10ms, runs 10s
 * Is the queued time for that operation 5ms, 10ms or 25ms? The current
 * implementation will report 5ms. If it's 25ms, then we need to
 * subtract 20ms from the total exec time so as not to count it twice.
 * We can implement that later if it matters to anyone ...
 */
static void
cmd_original_times(lrmd_cmd_t * cmd)
{
    cmd->t_run = cmd->t_first_run;
    cmd->t_queue = cmd->t_first_queue;
}
#endif

static void
send_cmd_complete_notify(lrmd_cmd_t * cmd)
{
    int exec_time = 0;
    int queue_time = 0;
    xmlNode *notify = NULL;

#ifdef HAVE_SYS_TIMEB_H
    exec_time = time_diff_ms(NULL, &cmd->t_run);
    queue_time = time_diff_ms(&cmd->t_run, &cmd->t_queue);
#endif

    log_finished(cmd, exec_time, queue_time);

    /* if the first notify result for a cmd has already been sent earlier, and the
     * the option to only send notifies on result changes is set. Check to see
     * if the last result is the same as the new one. If so, suppress this update */
    if (cmd->first_notify_sent && (cmd->call_opts & lrmd_opt_notify_changes_only)) {
        if (cmd->last_notify_rc == cmd->exec_rc &&
            cmd->last_notify_op_status == cmd->lrmd_op_status) {

            /* only send changes */
            return;
        }

    }

    cmd->first_notify_sent = 1;
    cmd->last_notify_rc = cmd->exec_rc;
    cmd->last_notify_op_status = cmd->lrmd_op_status;

    notify = create_xml_node(NULL, T_LRMD_NOTIFY);

    crm_xml_add(notify, F_LRMD_ORIGIN, __FUNCTION__);
    crm_xml_add_int(notify, F_LRMD_TIMEOUT, cmd->timeout);
    crm_xml_add_int(notify, F_LRMD_RSC_INTERVAL, cmd->interval);
    crm_xml_add_int(notify, F_LRMD_RSC_START_DELAY, cmd->start_delay);
    crm_xml_add_int(notify, F_LRMD_EXEC_RC, cmd->exec_rc);
    crm_xml_add_int(notify, F_LRMD_OP_STATUS, cmd->lrmd_op_status);
    crm_xml_add_int(notify, F_LRMD_CALLID, cmd->call_id);
    crm_xml_add_int(notify, F_LRMD_RSC_DELETED, cmd->rsc_deleted);

#ifdef HAVE_SYS_TIMEB_H
    crm_xml_add_int(notify, F_LRMD_RSC_RUN_TIME, cmd->t_run.time);
    crm_xml_add_int(notify, F_LRMD_RSC_RCCHANGE_TIME, cmd->t_rcchange.time);
    crm_xml_add_int(notify, F_LRMD_RSC_EXEC_TIME, exec_time);
    crm_xml_add_int(notify, F_LRMD_RSC_QUEUE_TIME, queue_time);
#endif

    crm_xml_add(notify, F_LRMD_OPERATION, LRMD_OP_RSC_EXEC);
    crm_xml_add(notify, F_LRMD_RSC_ID, cmd->rsc_id);
    if(cmd->real_action) {
        crm_xml_add(notify, F_LRMD_RSC_ACTION, cmd->real_action);
    } else {
        crm_xml_add(notify, F_LRMD_RSC_ACTION, cmd->action);
    }
    crm_xml_add(notify, F_LRMD_RSC_USERDATA_STR, cmd->userdata_str);
    crm_xml_add(notify, F_LRMD_RSC_OUTPUT, cmd->output);
    crm_xml_add(notify, F_LRMD_RSC_EXIT_REASON, cmd->exit_reason);

    if (cmd->params) {
        char *key = NULL;
        char *value = NULL;
        GHashTableIter iter;

        xmlNode *args = create_xml_node(notify, XML_TAG_ATTRS);

        g_hash_table_iter_init(&iter, cmd->params);
        while (g_hash_table_iter_next(&iter, (gpointer *) & key, (gpointer *) & value)) {
            hash2smartfield((gpointer) key, (gpointer) value, args);
        }
    }

    if (cmd->client_id && (cmd->call_opts & lrmd_opt_notify_orig_only)) {
        crm_client_t *client = crm_client_get_by_id(cmd->client_id);

        if (client) {
            send_client_notify(client->id, client, notify);
        }
    } else {
        g_hash_table_foreach(client_connections, send_client_notify, notify);
    }

    free_xml(notify);
}

static void
send_generic_notify(int rc, xmlNode * request)
{
    int call_id = 0;
    xmlNode *notify = NULL;
    xmlNode *rsc_xml = get_xpath_object("//" F_LRMD_RSC, request, LOG_ERR);
    const char *rsc_id = crm_element_value(rsc_xml, F_LRMD_RSC_ID);
    const char *op = crm_element_value(request, F_LRMD_OPERATION);

    crm_element_value_int(request, F_LRMD_CALLID, &call_id);

    notify = create_xml_node(NULL, T_LRMD_NOTIFY);
    crm_xml_add(notify, F_LRMD_ORIGIN, __FUNCTION__);
    crm_xml_add_int(notify, F_LRMD_RC, rc);
    crm_xml_add_int(notify, F_LRMD_CALLID, call_id);
    crm_xml_add(notify, F_LRMD_OPERATION, op);
    crm_xml_add(notify, F_LRMD_RSC_ID, rsc_id);

    g_hash_table_foreach(client_connections, send_client_notify, notify);

    free_xml(notify);
}

static void
cmd_reset(lrmd_cmd_t * cmd)
{
    cmd->lrmd_op_status = 0;
    cmd->last_pid = 0;
    memset(&cmd->t_run, 0, sizeof(cmd->t_run));
    memset(&cmd->t_queue, 0, sizeof(cmd->t_queue));
    free(cmd->exit_reason);
    cmd->exit_reason = NULL;
    free(cmd->output);
    cmd->output = NULL;

}

static void
cmd_finalize(lrmd_cmd_t * cmd, lrmd_rsc_t * rsc)
{
    crm_trace("Resource operation rsc:%s action:%s completed (%p %p)", cmd->rsc_id, cmd->action,
              rsc ? rsc->active : NULL, cmd);

    if (rsc && (rsc->active == cmd)) {
        rsc->active = NULL;
        mainloop_set_trigger(rsc->work);
    }

    if (!rsc) {
        cmd->rsc_deleted = 1;
    }

    /* reset original timeout so client notification has correct information */
    cmd->timeout = cmd->timeout_orig;

    send_cmd_complete_notify(cmd);

    if (cmd->interval && (cmd->lrmd_op_status == PCMK_LRM_OP_CANCELLED)) {
        if (rsc) {
            rsc->recurring_ops = g_list_remove(rsc->recurring_ops, cmd);
            rsc->pending_ops = g_list_remove(rsc->pending_ops, cmd);
        }
        free_lrmd_cmd(cmd);
    } else if (cmd->interval == 0) {
        if (rsc) {
            rsc->pending_ops = g_list_remove(rsc->pending_ops, cmd);
        }
        free_lrmd_cmd(cmd);
    } else {
        /* Clear all the values pertaining just to the last iteration of a recurring op. */
        cmd_reset(cmd);
    }
}

#if SUPPORT_HEARTBEAT
static int pattern_matched(const char *pat, const char *str)
{
    if (g_pattern_match_simple(pat, str)) {
        crm_debug("RA output matched stopped pattern [%s]", pat);
        return TRUE;
    }
    return FALSE;
}

static int
hb2uniform_rc(const char *action, int rc, const char *stdout_data)
{
    const char *stop_pattern[] = { "*stopped*", "*not*running*" };
    const char *running_pattern[] = { "*running*", "*OK*" };
    char *lower_std_output = NULL;
    int result;


    if (rc < 0) {
        return PCMK_OCF_UNKNOWN_ERROR;
    }

    /* Treat class heartbeat the same as class lsb. */
    if (!safe_str_eq(action, "status") && !safe_str_eq(action, "monitor")) {
        return services_get_ocf_exitcode(action, rc);
    }

    /* for status though, exit code is ignored,
     * and the stdout is scanned for specific strings */
    if (stdout_data == NULL) {
        crm_warn("No status output from the (hb) resource agent, assuming stopped");
        return PCMK_OCF_NOT_RUNNING;
    }

    lower_std_output = g_ascii_strdown(stdout_data, -1);

    if (pattern_matched(stop_pattern[0], lower_std_output) ||
        pattern_matched(stop_pattern[1], lower_std_output)) {
        result = PCMK_OCF_NOT_RUNNING;
    } else if (pattern_matched(running_pattern[0], lower_std_output) ||
        pattern_matched(running_pattern[1], stdout_data)) {
            /* "OK" is matched case sensitive */
        result = PCMK_OCF_OK;
    } else {
        /* It didn't say it was running - must be stopped */
        crm_debug("RA output did not match any pattern, assuming stopped");
        result = PCMK_OCF_NOT_RUNNING;
    }
    free(lower_std_output);
    return result;
}
#endif

static int
ocf2uniform_rc(int rc)
{
    if (rc < 0 || rc > PCMK_OCF_FAILED_MASTER) {
        return PCMK_OCF_UNKNOWN_ERROR;
    }

    return rc;
}

static int
stonith2uniform_rc(const char *action, int rc)
{
    if (rc == -ENODEV) {
        if (safe_str_eq(action, "stop")) {
            rc = PCMK_OCF_OK;
        } else if (safe_str_eq(action, "start")) {
            rc = PCMK_OCF_NOT_INSTALLED;
        } else {
            rc = PCMK_OCF_NOT_RUNNING;
        }
    } else if (rc != 0) {
        rc = PCMK_OCF_UNKNOWN_ERROR;
    }
    return rc;
}

#if SUPPORT_NAGIOS
static int
nagios2uniform_rc(const char *action, int rc)
{
    if (rc < 0) {
        return PCMK_OCF_UNKNOWN_ERROR;
    }

    switch (rc) {
        case NAGIOS_STATE_OK:
            return PCMK_OCF_OK;
        case NAGIOS_INSUFFICIENT_PRIV:
            return PCMK_OCF_INSUFFICIENT_PRIV;
        case NAGIOS_NOT_INSTALLED:
            return PCMK_OCF_NOT_INSTALLED;
        case NAGIOS_STATE_WARNING:
        case NAGIOS_STATE_CRITICAL:
        case NAGIOS_STATE_UNKNOWN:
        case NAGIOS_STATE_DEPENDENT:
        default:
            return PCMK_OCF_UNKNOWN_ERROR;
    }

    return PCMK_OCF_UNKNOWN_ERROR;
}
#endif

static int
get_uniform_rc(const char *standard, const char *action, int rc)
{
    if (safe_str_eq(standard, "ocf")) {
        return ocf2uniform_rc(rc);
    } else if (safe_str_eq(standard, "stonith")) {
        return stonith2uniform_rc(action, rc);
    } else if (safe_str_eq(standard, "systemd")) {
        return rc;
    } else if (safe_str_eq(standard, "upstart")) {
        return rc;
#if SUPPORT_NAGIOS
    } else if (safe_str_eq(standard, "nagios")) {
        return nagios2uniform_rc(action, rc);
#endif
    } else {
        return services_get_ocf_exitcode(action, rc);
    }
}

static int
action_get_uniform_rc(svc_action_t * action)
{
    lrmd_cmd_t *cmd = action->cb_data;
#if SUPPORT_HEARTBEAT
    if (safe_str_eq(action->standard, "heartbeat")) {
        return hb2uniform_rc(cmd->action, action->rc, action->stdout_data);
    }
#endif
    return get_uniform_rc(action->standard, cmd->action, action->rc);
}

void
notify_of_new_client(crm_client_t *new_client)
{
    crm_client_t *client = NULL;
    GHashTableIter iter;
    xmlNode *notify = NULL;
    char *key = NULL;

    notify = create_xml_node(NULL, T_LRMD_NOTIFY);
    crm_xml_add(notify, F_LRMD_ORIGIN, __FUNCTION__);
    crm_xml_add(notify, F_LRMD_OPERATION, LRMD_OP_NEW_CLIENT);

    g_hash_table_iter_init(&iter, client_connections);
    while (g_hash_table_iter_next(&iter, (gpointer *) & key, (gpointer *) & client)) {

        if (safe_str_eq(client->id, new_client->id)) {
            continue;
        }

        send_client_notify((gpointer) key, (gpointer) client, (gpointer) notify);
    }
    free_xml(notify);
}

static char *
parse_exit_reason(const char *output)
{
    const char *cur = NULL;
    const char *last = NULL;
    char *reason = NULL;
    static int cookie_len = 0;
    char *eol = NULL;

    if (output == NULL) {
        return NULL;
    }

    if (!cookie_len) {
        cookie_len = strlen(PCMK_OCF_REASON_PREFIX);
    }

    cur = strstr(output, PCMK_OCF_REASON_PREFIX);
    for (; cur != NULL; cur = strstr(cur, PCMK_OCF_REASON_PREFIX)) {
        /* skip over the cookie delimiter string */
        cur += cookie_len;
        last = cur;
    }
    if (last == NULL) {
        return NULL;
    }

    /* make our own copy */
    reason = calloc(1, (EXIT_REASON_MAX_LEN+1));
    CRM_ASSERT(reason);

    /* limit reason string size */
    strncpy(reason, last, EXIT_REASON_MAX_LEN);

    /* truncate everything after a new line */
    eol = strchr(reason, '\n');
    if (eol != NULL) {
        *eol = '\0';
    }

    return reason;
}

void
client_disconnect_cleanup(const char *client_id)
{
    GHashTableIter iter;
    lrmd_rsc_t *rsc = NULL;
    char *key = NULL;

    g_hash_table_iter_init(&iter, rsc_list);
    while (g_hash_table_iter_next(&iter, (gpointer *) & key, (gpointer *) & rsc)) {
        if (rsc->call_opts & lrmd_opt_drop_recurring) {
            /* This client is disconnecting, drop any recurring operations
             * it may have initiated on the resource */
            cancel_all_recurring(rsc, client_id);
        }
    }
}

static void
action_complete(svc_action_t * action)
{
    lrmd_rsc_t *rsc;
    lrmd_cmd_t *cmd = action->cb_data;
    const char *rclass = NULL;

    bool goagain = false;

    if (!cmd) {
        crm_err("LRMD action (%s) completed does not match any known operations.", action->id);
        return;
    }
#ifdef HAVE_SYS_TIMEB_H
    if (cmd->exec_rc != action->rc) {
        ftime(&cmd->t_rcchange);
    }
#endif

    cmd->last_pid = action->pid;
    cmd->exec_rc = action_get_uniform_rc(action);
    cmd->lrmd_op_status = action->status;
    rsc = cmd->rsc_id ? g_hash_table_lookup(rsc_list, cmd->rsc_id) : NULL;

    if(rsc && safe_str_eq(rsc->class, "service")) {
        rclass = resources_find_service_class(rsc->class);
    } else if(rsc) {
        rclass = rsc->class;
    }

    if (safe_str_eq(rclass, "systemd")) {
        if(cmd->exec_rc == PCMK_OCF_OK && safe_str_eq(cmd->action, "start")) {
            /* systemd I curse thee!
             *
             * systemd returns from start actions after the start _begins_
             * not after it completes.
             *
             * So we have to jump through a few hoops so that we don't
             * report 'complete' to the rest of pacemaker until, you know,
             * its actually done.
             */
            goagain = true;
            cmd->real_action = cmd->action;
            cmd->action = strdup("monitor");

        } else if(cmd->exec_rc == PCMK_OCF_OK && safe_str_eq(cmd->action, "stop")) {
            goagain = true;
            cmd->real_action = cmd->action;
            cmd->action = strdup("monitor");

        } else if(cmd->real_action) {
            /* Ok, so this is the follow up monitor action to check if start actually completed */
            if(cmd->lrmd_op_status == PCMK_LRM_OP_DONE && cmd->exec_rc == PCMK_OCF_PENDING) {
                goagain = true;

            } else {
#ifdef HAVE_SYS_TIMEB_H
                int time_sum = time_diff_ms(NULL, &cmd->t_first_run);
                int timeout_left = cmd->timeout_orig - time_sum;

                crm_debug("%s %s is now complete (elapsed=%dms, remaining=%dms): %s (%d)",
                          cmd->rsc_id, cmd->real_action, time_sum, timeout_left, services_ocf_exitcode_str(cmd->exec_rc), cmd->exec_rc);
                cmd_original_times(cmd);
#endif

                if(cmd->lrmd_op_status == PCMK_LRM_OP_DONE && cmd->exec_rc == PCMK_OCF_NOT_RUNNING && safe_str_eq(cmd->real_action, "stop")) {
                    cmd->exec_rc = PCMK_OCF_OK;
                }
            }
        }
    }

#if SUPPORT_NAGIOS
    if (rsc && safe_str_eq(rsc->class, "nagios")) {
        if (safe_str_eq(cmd->action, "monitor") &&
            cmd->interval == 0 && cmd->exec_rc == PCMK_OCF_OK) {
            /* Successfully executed --version for the nagios plugin */
            cmd->exec_rc = PCMK_OCF_NOT_RUNNING;

        } else if (safe_str_eq(cmd->action, "start") && cmd->exec_rc != PCMK_OCF_OK) {
            goagain = true;
        }
    }
#endif

    /* Wrapping this section in ifdef implies that systemd resources are not
     * fully supported on platforms without sys/timeb.h. Since timeb is
     * obsolete, we should eventually prefer a clock_gettime() implementation
     * (wrapped in its own ifdef) with timeb as a fallback.
     */
#ifdef HAVE_SYS_TIMEB_H
    if(goagain) {
        int time_sum = time_diff_ms(NULL, &cmd->t_first_run);
        int timeout_left = cmd->timeout_orig - time_sum;
        int delay = cmd->timeout_orig / 10;

        if(delay >= timeout_left && timeout_left > 20) {
            delay = timeout_left/2;
        }

        delay = QB_MIN(2000, delay);
        if (delay < timeout_left) {
            cmd->start_delay = delay;
            cmd->timeout = timeout_left;

            if(cmd->exec_rc == PCMK_OCF_OK) {
                crm_debug("%s %s may still be in progress: re-scheduling (elapsed=%dms, remaining=%dms, start_delay=%dms)",
                          cmd->rsc_id, cmd->real_action, time_sum, timeout_left, delay);

            } else if(cmd->exec_rc == PCMK_OCF_PENDING) {
                crm_info("%s %s is still in progress: re-scheduling (elapsed=%dms, remaining=%dms, start_delay=%dms)",
                         cmd->rsc_id, cmd->action, time_sum, timeout_left, delay);

            } else {
                crm_notice("%s %s failed '%s' (%d): re-scheduling (elapsed=%dms, remaining=%dms, start_delay=%dms)",
                           cmd->rsc_id, cmd->action, services_ocf_exitcode_str(cmd->exec_rc), cmd->exec_rc, time_sum, timeout_left, delay);
            }

            cmd_reset(cmd);
            if(rsc) {
                rsc->active = NULL;
            }
            schedule_lrmd_cmd(rsc, cmd);

            /* Don't finalize cmd, we're not done with it yet */
            return;

        } else {
            crm_notice("Giving up on %s %s (rc=%d): timeout (elapsed=%dms, remaining=%dms)",
                       cmd->rsc_id, cmd->real_action?cmd->real_action:cmd->action, cmd->exec_rc, time_sum, timeout_left);
            cmd->lrmd_op_status = PCMK_LRM_OP_TIMEOUT;
            cmd->exec_rc = PCMK_OCF_TIMEOUT;
            cmd_original_times(cmd);
        }
    }
#endif

    if (action->stderr_data) {
        cmd->output = strdup(action->stderr_data);
        cmd->exit_reason = parse_exit_reason(action->stderr_data);

    } else if (action->stdout_data) {
        cmd->output = strdup(action->stdout_data);
    }

    cmd_finalize(cmd, rsc);
}

static void
stonith_action_complete(lrmd_cmd_t * cmd, int rc)
{
    int recurring = cmd->interval;
    lrmd_rsc_t *rsc = NULL;

    cmd->exec_rc = get_uniform_rc("stonith", cmd->action, rc);

    rsc = g_hash_table_lookup(rsc_list, cmd->rsc_id);

    if (cmd->lrmd_op_status == PCMK_LRM_OP_CANCELLED) {
        recurring = 0;
        /* do nothing */

    } else if (rc == -ENODEV && safe_str_eq(cmd->action, "monitor")) {
        /* Not registered == inactive */
        cmd->lrmd_op_status = PCMK_LRM_OP_DONE;
        cmd->exec_rc = PCMK_OCF_NOT_RUNNING;

    } else if (rc) {
        /* Attempt to map return codes to op status if possible */
        switch (rc) {
            case -EPROTONOSUPPORT:
                cmd->lrmd_op_status = PCMK_LRM_OP_NOTSUPPORTED;
                break;
            case -ETIME:
                cmd->lrmd_op_status = PCMK_LRM_OP_TIMEOUT;
                break;
            default:
                /* TODO: This looks wrong.  Status should be _DONE and exec_rc set to an error */
                cmd->lrmd_op_status = PCMK_LRM_OP_ERROR;
        }
    } else {
        /* command successful */
        cmd->lrmd_op_status = PCMK_LRM_OP_DONE;
        if (safe_str_eq(cmd->action, "start") && rsc) {
            rsc->stonith_started = 1;
        }
    }

    if (recurring && rsc) {
        if (cmd->stonith_recurring_id) {
            g_source_remove(cmd->stonith_recurring_id);
        }
        cmd->stonith_recurring_id = g_timeout_add(cmd->interval, stonith_recurring_op_helper, cmd);
    }

    cmd_finalize(cmd, rsc);
}

static void
lrmd_stonith_callback(stonith_t * stonith, stonith_callback_data_t * data)
{
    stonith_action_complete(data->userdata, data->rc);
}

void
stonith_connection_failed(void)
{
    GHashTableIter iter;
    GList *cmd_list = NULL;
    GList *cmd_iter = NULL;
    lrmd_rsc_t *rsc = NULL;
    char *key = NULL;

    g_hash_table_iter_init(&iter, rsc_list);
    while (g_hash_table_iter_next(&iter, (gpointer *) & key, (gpointer *) & rsc)) {
        if (safe_str_eq(rsc->class, "stonith")) {
            if (rsc->active) {
                cmd_list = g_list_append(cmd_list, rsc->active);
            }
            if (rsc->recurring_ops) {
                cmd_list = g_list_concat(cmd_list, rsc->recurring_ops);
            }
            if (rsc->pending_ops) {
                cmd_list = g_list_concat(cmd_list, rsc->pending_ops);
            }
            rsc->pending_ops = rsc->recurring_ops = NULL;
        }
    }

    if (!cmd_list) {
        return;
    }

    crm_err("STONITH connection failed, finalizing %d pending operations.",
            g_list_length(cmd_list));
    for (cmd_iter = cmd_list; cmd_iter; cmd_iter = cmd_iter->next) {
        stonith_action_complete(cmd_iter->data, -ENOTCONN);
    }
    g_list_free(cmd_list);
}

static int
lrmd_rsc_execute_stonith(lrmd_rsc_t * rsc, lrmd_cmd_t * cmd)
{
    int rc = 0;
    int do_monitor = 0;

    stonith_t *stonith_api = get_stonith_connection();

    if (!stonith_api) {
        cmd->exec_rc = get_uniform_rc("stonith", cmd->action, -ENOTCONN);
        cmd->lrmd_op_status = PCMK_LRM_OP_ERROR;
        cmd_finalize(cmd, rsc);
        return -EUNATCH;
    }

    if (safe_str_eq(cmd->action, "start")) {
        char *key = NULL;
        char *value = NULL;
        stonith_key_value_t *device_params = NULL;

        if (cmd->params) {
            GHashTableIter iter;

            g_hash_table_iter_init(&iter, cmd->params);
            while (g_hash_table_iter_next(&iter, (gpointer *) & key, (gpointer *) & value)) {
                device_params = stonith_key_value_add(device_params, key, value);
            }
        }

        /* Stonith automatically registers devices from the IPC when changes occur,
         * but to avoid a possible race condition between stonith receiving the IPC update
         * and the lrmd requesting that resource, the lrmd still registers the device as well.
         * Stonith knows how to handle duplicate device registrations correctly. */
        rc = stonith_api->cmds->register_device(stonith_api,
                                                st_opt_sync_call,
                                                cmd->rsc_id,
                                                rsc->provider, rsc->type, device_params);

        stonith_key_value_freeall(device_params, 1, 1);
        if (rc == 0) {
            do_monitor = 1;
        }
    } else if (safe_str_eq(cmd->action, "stop")) {
        rc = stonith_api->cmds->remove_device(stonith_api, st_opt_sync_call, cmd->rsc_id);
        rsc->stonith_started = 0;
    } else if (safe_str_eq(cmd->action, "monitor")) {
        if (cmd->interval) {
            do_monitor = 1;
        } else {
            rc = rsc->stonith_started ? 0 : -ENODEV;
        }
    }

    if (!do_monitor) {
        goto cleanup_stonith_exec;
    }

    rc = stonith_api->cmds->monitor(stonith_api, 0, cmd->rsc_id, cmd->timeout / 1000);

    rc = stonith_api->cmds->register_callback(stonith_api,
                                              rc,
                                              0,
                                              0,
                                              cmd, "lrmd_stonith_callback", lrmd_stonith_callback);

    /* don't cleanup yet, we will find out the result of the monitor later */
    if (rc > 0) {
        rsc->active = cmd;
        return rc;
    } else if (rc == 0) {
        rc = -1;
    }

  cleanup_stonith_exec:
    stonith_action_complete(cmd, rc);
    return rc;
}

static void
dup_attr(gpointer key, gpointer value, gpointer user_data)
{
    g_hash_table_replace(user_data, strdup(key), strdup(value));
}

static int
lrmd_rsc_execute_service_lib(lrmd_rsc_t * rsc, lrmd_cmd_t * cmd)
{
    svc_action_t *action = NULL;
    GHashTable *params_copy = NULL;

    CRM_ASSERT(rsc);
    CRM_ASSERT(cmd);

    crm_trace("Creating action, resource:%s action:%s class:%s provider:%s agent:%s",
              rsc->rsc_id, cmd->action, rsc->class, rsc->provider, rsc->type);

#if SUPPORT_NAGIOS
    /* Recurring operations are cancelled anyway for a stop operation */
    if (safe_str_eq(rsc->class, "nagios") && safe_str_eq(cmd->action, "stop")) {
        cmd->exec_rc = PCMK_OCF_OK;
        goto exec_done;
    }
#endif

    if (cmd->params) {
        params_copy = g_hash_table_new_full(crm_str_hash,
                                            g_str_equal, g_hash_destroy_str, g_hash_destroy_str);

        if (params_copy != NULL) {
            g_hash_table_foreach(cmd->params, dup_attr, params_copy);
        }
    }

    if (cmd->isolation_wrapper) {
        g_hash_table_remove(params_copy, "CRM_meta_isolation_wrapper");
        action = resources_action_create(rsc->rsc_id,
                                         "ocf",
                                         LRMD_ISOLATION_PROVIDER,
                                         cmd->isolation_wrapper,
                                         cmd->action, /*action will be normalized in wrapper*/
                                         cmd->interval,
                                         cmd->timeout,
                                         params_copy,
                                         cmd->service_flags);
    } else {
        action = resources_action_create(rsc->rsc_id,
                                         rsc->class,
                                         rsc->provider,
                                         rsc->type,
                                         normalize_action_name(rsc, cmd->action),
                                         cmd->interval,
                                         cmd->timeout,
                                         params_copy,
                                         cmd->service_flags);
    }

    if (!action) {
        crm_err("Failed to create action, action:%s on resource %s", cmd->action, rsc->rsc_id);
        cmd->lrmd_op_status = PCMK_LRM_OP_ERROR;
        goto exec_done;
    }

    action->cb_data = cmd;

    /* 'cmd' may not be valid after this point if
     * services_action_async() returned TRUE
     *
     * Upstart and systemd both synchronously determine monitor/status
     * results and call action_complete (which may free 'cmd') if necessary.
     */
    if (services_action_async(action, action_complete)) {
        return TRUE;
    }

    cmd->exec_rc = action->rc;
    if(action->status != PCMK_LRM_OP_DONE) {
        cmd->lrmd_op_status = action->status;
    } else {
        cmd->lrmd_op_status = PCMK_LRM_OP_ERROR;
    }
    services_action_free(action);
    action = NULL;

  exec_done:
    cmd_finalize(cmd, rsc);
    return TRUE;
}

static gboolean
lrmd_rsc_execute(lrmd_rsc_t * rsc)
{
    lrmd_cmd_t *cmd = NULL;

    CRM_CHECK(rsc != NULL, return FALSE);

    if (rsc->active) {
        crm_trace("%s is still active", rsc->rsc_id);
        return TRUE;
    }

    if (rsc->pending_ops) {
        GList *first = rsc->pending_ops;

        cmd = first->data;
        if (cmd->delay_id) {
            crm_trace
                ("Command %s %s was asked to run too early, waiting for start_delay timeout of %dms",
                 cmd->rsc_id, cmd->action, cmd->start_delay);
            return TRUE;
        }
        rsc->pending_ops = g_list_remove_link(rsc->pending_ops, first);
        g_list_free_1(first);

#ifdef HAVE_SYS_TIMEB_H
        if (cmd->t_first_run.time == 0) {
            ftime(&cmd->t_first_run);
        }
        ftime(&cmd->t_run);
#endif
    }

    if (!cmd) {
        crm_trace("Nothing further to do for %s", rsc->rsc_id);
        return TRUE;
    }

    rsc->active = cmd;          /* only one op at a time for a rsc */
    if (cmd->interval) {
        rsc->recurring_ops = g_list_append(rsc->recurring_ops, cmd);
    }

    log_execute(cmd);

    if (safe_str_eq(rsc->class, "stonith")) {
        lrmd_rsc_execute_stonith(rsc, cmd);
    } else {
        lrmd_rsc_execute_service_lib(rsc, cmd);
    }

    return TRUE;
}

static gboolean
lrmd_rsc_dispatch(gpointer user_data)
{
    return lrmd_rsc_execute(user_data);
}

void
free_rsc(gpointer data)
{
    GListPtr gIter = NULL;
    lrmd_rsc_t *rsc = data;
    int is_stonith = safe_str_eq(rsc->class, "stonith");

    gIter = rsc->pending_ops;
    while (gIter != NULL) {
        GListPtr next = gIter->next;
        lrmd_cmd_t *cmd = gIter->data;

        /* command was never executed */
        cmd->lrmd_op_status = PCMK_LRM_OP_CANCELLED;
        cmd_finalize(cmd, NULL);

        gIter = next;
    }
    /* frees list, but not list elements. */
    g_list_free(rsc->pending_ops);

    gIter = rsc->recurring_ops;
    while (gIter != NULL) {
        GListPtr next = gIter->next;
        lrmd_cmd_t *cmd = gIter->data;

        if (is_stonith) {
            cmd->lrmd_op_status = PCMK_LRM_OP_CANCELLED;
            /* if a stonith cmd is in-flight, mark just mark it as cancelled,
             * it is not safe to finalize/free the cmd until the stonith api
             * says it has either completed or timed out.*/ 
            if (rsc->active != cmd) {
                cmd_finalize(cmd, NULL);
            }
        } else {
            /* This command is already handed off to service library,
             * let service library cancel it and tell us via the callback
             * when it is cancelled. The rsc can be safely destroyed
             * even if we are waiting for the cancel result */
            services_action_cancel(rsc->rsc_id, normalize_action_name(rsc, cmd->action), cmd->interval);
        }

        gIter = next;
    }
    /* frees list, but not list elements. */
    g_list_free(rsc->recurring_ops);

    free(rsc->rsc_id);
    free(rsc->class);
    free(rsc->provider);
    free(rsc->type);
    mainloop_destroy_trigger(rsc->work);

    free(rsc);
}

static int
process_lrmd_signon(crm_client_t * client, uint32_t id, xmlNode * request)
{
    xmlNode *reply = create_xml_node(NULL, "reply");
    const char *is_ipc_provider = crm_element_value(request, F_LRMD_IS_IPC_PROVIDER);
    const char *protocol_version = crm_element_value(request, F_LRMD_PROTOCOL_VERSION);

    if (safe_str_neq(protocol_version, LRMD_PROTOCOL_VERSION)) {
        crm_xml_add_int(reply, F_LRMD_RC, -EPROTO);
        crm_xml_add(reply, F_LRMD_PROTOCOL_VERSION, LRMD_PROTOCOL_VERSION);
    }

    crm_xml_add(reply, F_LRMD_OPERATION, CRM_OP_REGISTER);
    crm_xml_add(reply, F_LRMD_CLIENTID, client->id);
    lrmd_server_send_reply(client, id, reply);

    if (crm_is_true(is_ipc_provider)) {
        /* this is a remote connection from a cluster nodes crmd */
#ifdef SUPPORT_REMOTE
        ipc_proxy_add_provider(client);
#endif
    }

    free_xml(reply);
    return pcmk_ok;
}

static int
process_lrmd_rsc_register(crm_client_t * client, uint32_t id, xmlNode * request)
{
    int rc = pcmk_ok;
    lrmd_rsc_t *rsc = build_rsc_from_xml(request);
    lrmd_rsc_t *dup = g_hash_table_lookup(rsc_list, rsc->rsc_id);

    if (dup &&
        safe_str_eq(rsc->class, dup->class) &&
        safe_str_eq(rsc->provider, dup->provider) && safe_str_eq(rsc->type, dup->type)) {

        crm_warn("Can't add, RSC '%s' already present in the rsc list (%d active resources)",
                 rsc->rsc_id, g_hash_table_size(rsc_list));

        free_rsc(rsc);
        return rc;
    }

    g_hash_table_replace(rsc_list, rsc->rsc_id, rsc);
    crm_info("Added '%s' to the rsc list (%d active resources)",
             rsc->rsc_id, g_hash_table_size(rsc_list));

    return rc;
}

static void
process_lrmd_get_rsc_info(crm_client_t * client, uint32_t id, xmlNode * request)
{
    int rc = pcmk_ok;
    int send_rc = 0;
    int call_id = 0;
    xmlNode *rsc_xml = get_xpath_object("//" F_LRMD_RSC, request, LOG_ERR);
    const char *rsc_id = crm_element_value(rsc_xml, F_LRMD_RSC_ID);
    xmlNode *reply = NULL;
    lrmd_rsc_t *rsc = NULL;

    crm_element_value_int(request, F_LRMD_CALLID, &call_id);

    if (!rsc_id) {
        rc = -ENODEV;
        goto get_rsc_done;
    }

    if (!(rsc = g_hash_table_lookup(rsc_list, rsc_id))) {
        crm_info("Resource '%s' not found (%d active resources)",
                 rsc_id, g_hash_table_size(rsc_list));
        rc = -ENODEV;
        goto get_rsc_done;
    }

  get_rsc_done:

    reply = create_xml_node(NULL, T_LRMD_REPLY);
    crm_xml_add(reply, F_LRMD_ORIGIN, __FUNCTION__);
    crm_xml_add_int(reply, F_LRMD_RC, rc);
    crm_xml_add_int(reply, F_LRMD_CALLID, call_id);

    if (rsc) {
        crm_xml_add(reply, F_LRMD_RSC_ID, rsc->rsc_id);
        crm_xml_add(reply, F_LRMD_CLASS, rsc->class);
        crm_xml_add(reply, F_LRMD_PROVIDER, rsc->provider);
        crm_xml_add(reply, F_LRMD_TYPE, rsc->type);
    }

    send_rc = lrmd_server_send_reply(client, id, reply);

    if (send_rc < 0) {
        crm_warn("LRMD reply to %s failed: %d", client->name, send_rc);
    }

    free_xml(reply);
}

static int
process_lrmd_rsc_unregister(crm_client_t * client, uint32_t id, xmlNode * request)
{
    int rc = pcmk_ok;
    lrmd_rsc_t *rsc = NULL;
    xmlNode *rsc_xml = get_xpath_object("//" F_LRMD_RSC, request, LOG_ERR);
    const char *rsc_id = crm_element_value(rsc_xml, F_LRMD_RSC_ID);

    if (!rsc_id) {
        return -ENODEV;
    }

    if (!(rsc = g_hash_table_lookup(rsc_list, rsc_id))) {
        crm_info("Resource '%s' not found (%d active resources)",
                 rsc_id, g_hash_table_size(rsc_list));
        return pcmk_ok;
    }

    if (rsc->active) {
        /* let the caller know there are still active ops on this rsc to watch for */
        crm_trace("Operation still in progress: %p", rsc->active);
        rc = -EINPROGRESS;
    }

    g_hash_table_remove(rsc_list, rsc_id);

    return rc;
}

static int
process_lrmd_rsc_exec(crm_client_t * client, uint32_t id, xmlNode * request)
{
    lrmd_rsc_t *rsc = NULL;
    lrmd_cmd_t *cmd = NULL;
    xmlNode *rsc_xml = get_xpath_object("//" F_LRMD_RSC, request, LOG_ERR);
    const char *rsc_id = crm_element_value(rsc_xml, F_LRMD_RSC_ID);
    int call_id;

    if (!rsc_id) {
        return -EINVAL;
    }
    if (!(rsc = g_hash_table_lookup(rsc_list, rsc_id))) {
        crm_info("Resource '%s' not found (%d active resources)",
                 rsc_id, g_hash_table_size(rsc_list));
        return -ENODEV;
    }

    cmd = create_lrmd_cmd(request, client, rsc);
    call_id = cmd->call_id;

    /* Don't reference cmd after handing it off to be scheduled.
     * The cmd could get merged and freed. */
    schedule_lrmd_cmd(rsc, cmd);

    return call_id;
}

static int
cancel_op(const char *rsc_id, const char *action, int interval)
{
    GListPtr gIter = NULL;
    lrmd_rsc_t *rsc = g_hash_table_lookup(rsc_list, rsc_id);

    /* How to cancel an action.
     * 1. Check pending ops list, if it hasn't been handed off
     *    to the service library or stonith recurring list remove
     *    it there and that will stop it.
     * 2. If it isn't in the pending ops list, then its either a
     *    recurring op in the stonith recurring list, or the service
     *    library's recurring list.  Stop it there
     * 3. If not found in any lists, then this operation has either
     *    been executed already and is not a recurring operation, or
     *    never existed.
     */
    if (!rsc) {
        return -ENODEV;
    }

    for (gIter = rsc->pending_ops; gIter != NULL; gIter = gIter->next) {
        lrmd_cmd_t *cmd = gIter->data;

        if (safe_str_eq(cmd->action, action) && cmd->interval == interval) {
            cmd->lrmd_op_status = PCMK_LRM_OP_CANCELLED;
            cmd_finalize(cmd, rsc);
            return pcmk_ok;
        }
    }

    if (safe_str_eq(rsc->class, "stonith")) {
        /* The service library does not handle stonith operations.
         * We have to handle recurring stonith opereations ourselves. */
        for (gIter = rsc->recurring_ops; gIter != NULL; gIter = gIter->next) {
            lrmd_cmd_t *cmd = gIter->data;

            if (safe_str_eq(cmd->action, action) && cmd->interval == interval) {
                cmd->lrmd_op_status = PCMK_LRM_OP_CANCELLED;
                if (rsc->active != cmd) {
                    cmd_finalize(cmd, rsc);
                }
                return pcmk_ok;
            }
        }
    } else if (services_action_cancel(rsc_id, normalize_action_name(rsc, action), interval) == TRUE) {
        /* The service library will tell the action_complete callback function
         * this action was cancelled, which will destroy the cmd and remove
         * it from the recurring_op list. Do not do that in this function
         * if the service library says it cancelled it. */
        return pcmk_ok;
    }

    return -EOPNOTSUPP;
}

static void
cancel_all_recurring(lrmd_rsc_t * rsc, const char *client_id)
{
    GList *cmd_list = NULL;
    GList *cmd_iter = NULL;

    /* Notice a copy of each list is created when concat is called.
     * This prevents odd behavior from occurring when the cmd_list
     * is iterated through later on.  It is possible the cancel_op
     * function may end up modifying the recurring_ops and pending_ops
     * lists.  If we did not copy those lists, our cmd_list iteration
     * could get messed up.*/
    if (rsc->recurring_ops) {
        cmd_list = g_list_concat(cmd_list, g_list_copy(rsc->recurring_ops));
    }
    if (rsc->pending_ops) {
        cmd_list = g_list_concat(cmd_list, g_list_copy(rsc->pending_ops));
    }
    if (!cmd_list) {
        return;
    }

    for (cmd_iter = cmd_list; cmd_iter; cmd_iter = cmd_iter->next) {
        lrmd_cmd_t *cmd = cmd_iter->data;

        if (cmd->interval == 0) {
            continue;
        }

        if (client_id && safe_str_neq(cmd->client_id, client_id)) {
            continue;
        }

        cancel_op(rsc->rsc_id, cmd->action, cmd->interval);
    }
    /* frees only the copied list data, not the cmds */
    g_list_free(cmd_list);
}

static int
process_lrmd_rsc_cancel(crm_client_t * client, uint32_t id, xmlNode * request)
{
    xmlNode *rsc_xml = get_xpath_object("//" F_LRMD_RSC, request, LOG_ERR);
    const char *rsc_id = crm_element_value(rsc_xml, F_LRMD_RSC_ID);
    const char *action = crm_element_value(rsc_xml, F_LRMD_RSC_ACTION);
    int interval = 0;

    crm_element_value_int(rsc_xml, F_LRMD_RSC_INTERVAL, &interval);

    if (!rsc_id || !action) {
        return -EINVAL;
    }

    return cancel_op(rsc_id, action, interval);
}

void
process_lrmd_message(crm_client_t * client, uint32_t id, xmlNode * request)
{
    int rc = pcmk_ok;
    int call_id = 0;
    const char *op = crm_element_value(request, F_LRMD_OPERATION);
    int do_reply = 0;
    int do_notify = 0;

    crm_trace("Processing %s operation from %s", op, client->id);
    crm_element_value_int(request, F_LRMD_CALLID, &call_id);

    if (crm_str_eq(op, CRM_OP_IPC_FWD, TRUE)) {
#ifdef SUPPORT_REMOTE
        ipc_proxy_forward_client(client, request);
#endif
        do_reply = 1;
    } else if (crm_str_eq(op, CRM_OP_REGISTER, TRUE)) {
        rc = process_lrmd_signon(client, id, request);
    } else if (crm_str_eq(op, LRMD_OP_RSC_REG, TRUE)) {
        rc = process_lrmd_rsc_register(client, id, request);
        do_notify = 1;
        do_reply = 1;
    } else if (crm_str_eq(op, LRMD_OP_RSC_INFO, TRUE)) {
        process_lrmd_get_rsc_info(client, id, request);
    } else if (crm_str_eq(op, LRMD_OP_RSC_UNREG, TRUE)) {
        rc = process_lrmd_rsc_unregister(client, id, request);
        /* don't notify anyone about failed un-registers */
        if (rc == pcmk_ok || rc == -EINPROGRESS) {
            do_notify = 1;
        }
        do_reply = 1;
    } else if (crm_str_eq(op, LRMD_OP_RSC_EXEC, TRUE)) {
        rc = process_lrmd_rsc_exec(client, id, request);
        do_reply = 1;
    } else if (crm_str_eq(op, LRMD_OP_RSC_CANCEL, TRUE)) {
        rc = process_lrmd_rsc_cancel(client, id, request);
        do_reply = 1;
    } else if (crm_str_eq(op, LRMD_OP_POKE, TRUE)) {
        do_notify = 1;
        do_reply = 1;
    } else {
        rc = -EOPNOTSUPP;
        do_reply = 1;
        crm_err("Unknown %s from %s", op, client->name);
        crm_log_xml_warn(request, "UnknownOp");
    }

    crm_debug("Processed %s operation from %s: rc=%d, reply=%d, notify=%d, exit=%d",
              op, client->id, rc, do_reply, do_notify, exit);

    if (do_reply) {
        send_reply(client, rc, id, call_id);
    }

    if (do_notify) {
        send_generic_notify(rc, request);
    }
}
