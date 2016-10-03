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
#include <crm/crm.h>
#include <crm/cib.h>
#include <crm/msg_xml.h>

#include <crm/common/xml.h>
#include <tengine.h>

#include <crmd_fsa.h>
#include <crmd_messages.h>
#include <crm/cluster.h>
#include <throttle.h>

char *te_uuid = NULL;
GHashTable *te_targets = NULL;
void send_rsc_command(crm_action_t * action);
static void te_update_job_count(crm_action_t * action, int offset);

static void
te_start_action_timer(crm_graph_t * graph, crm_action_t * action)
{
    action->timer = calloc(1, sizeof(crm_action_timer_t));
    action->timer->timeout = action->timeout;
    action->timer->reason = timeout_action;
    action->timer->action = action;
    action->timer->source_id = g_timeout_add(action->timer->timeout + graph->network_delay,
                                             action_timer_callback, (void *)action->timer);

    CRM_ASSERT(action->timer->source_id != 0);
}

static gboolean
te_pseudo_action(crm_graph_t * graph, crm_action_t * pseudo)
{
    crm_debug("Pseudo action %d fired and confirmed", pseudo->id);
    te_action_confirmed(pseudo);
    update_graph(graph, pseudo);
    trigger_graph();
    return TRUE;
}

void
send_stonith_update(crm_action_t * action, const char *target, const char *uuid)
{
    int rc = pcmk_ok;
    crm_node_t *peer = NULL;

    /* zero out the node-status & remove all LRM status info */
    xmlNode *node_state = NULL;

    CRM_CHECK(target != NULL, return);
    CRM_CHECK(uuid != NULL, return);

    /* Make sure the membership and join caches are accurate */
    peer = crm_get_peer_full(0, target, CRM_GET_PEER_ANY);

    CRM_CHECK(peer != NULL, return);

    if (peer->uuid == NULL) {
        crm_info("Recording uuid '%s' for node '%s'", uuid, target);
        peer->uuid = strdup(uuid);
    }

    crmd_peer_down(peer, TRUE);

    /* Generate a node state update for the CIB.
     * We rely on the membership layer to do node_update_cluster,
     * and the peer status callback to do node_update_peer,
     * because the node might rejoin before we get the stonith result.
     */
    node_state = do_update_node_cib(peer, node_update_join|node_update_expected,
                                    NULL, __FUNCTION__);

    /* we have to mark whether or not remote nodes have already been fenced */
    if (peer->flags & crm_remote_node) {
        time_t now = time(NULL);
        char *now_s = crm_itoa(now);
        crm_xml_add(node_state, XML_NODE_IS_FENCED, now_s);
        free(now_s);
    }

    /* Force our known ID */
    crm_xml_add(node_state, XML_ATTR_UUID, uuid);

    rc = fsa_cib_conn->cmds->update(fsa_cib_conn, XML_CIB_TAG_STATUS, node_state,
                                    cib_quorum_override | cib_scope_local | cib_can_create);

    /* Delay processing the trigger until the update completes */
    crm_debug("Sending fencing update %d for %s", rc, target);
    fsa_register_cib_callback(rc, FALSE, strdup(target), cib_fencing_updated);

    /* Make sure it sticks */
    /* fsa_cib_conn->cmds->bump_epoch(fsa_cib_conn, cib_quorum_override|cib_scope_local);    */

    erase_status_tag(peer->uname, XML_CIB_TAG_LRM, cib_scope_local);
    erase_status_tag(peer->uname, XML_TAG_TRANSIENT_NODEATTRS, cib_scope_local);

    free_xml(node_state);
    return;
}

static gboolean
te_fence_node(crm_graph_t * graph, crm_action_t * action)
{
    int rc = 0;
    const char *id = NULL;
    const char *uuid = NULL;
    const char *target = NULL;
    const char *type = NULL;
    gboolean invalid_action = FALSE;
    enum stonith_call_options options = st_opt_none;

    id = ID(action->xml);
    target = crm_element_value(action->xml, XML_LRM_ATTR_TARGET);
    uuid = crm_element_value(action->xml, XML_LRM_ATTR_TARGET_UUID);
    type = crm_meta_value(action->params, "stonith_action");

    CRM_CHECK(id != NULL, invalid_action = TRUE);
    CRM_CHECK(uuid != NULL, invalid_action = TRUE);
    CRM_CHECK(type != NULL, invalid_action = TRUE);
    CRM_CHECK(target != NULL, invalid_action = TRUE);

    if (invalid_action) {
        crm_log_xml_warn(action->xml, "BadAction");
        return FALSE;
    }

    crm_notice("Executing %s fencing operation (%s) on %s (timeout=%d)",
               type, id, target, transition_graph->stonith_timeout);

    /* Passing NULL means block until we can connect... */
    te_connect_stonith(NULL);

    if (crmd_join_phase_count(crm_join_confirmed) == 1) {
        options |= st_opt_allow_suicide;
    }

    rc = stonith_api->cmds->fence(stonith_api, options, target, type,
                                  transition_graph->stonith_timeout / 1000, 0);

    stonith_api->cmds->register_callback(stonith_api, rc, transition_graph->stonith_timeout / 1000,
                                         st_opt_timeout_updates,
                                         generate_transition_key(transition_graph->id, action->id,
                                                                 0, te_uuid),
                                         "tengine_stonith_callback", tengine_stonith_callback);

    return TRUE;
}

static int
get_target_rc(crm_action_t * action)
{
    const char *target_rc_s = crm_meta_value(action->params, XML_ATTR_TE_TARGET_RC);

    if (target_rc_s != NULL) {
        return crm_parse_int(target_rc_s, "0");
    }
    return 0;
}

static gboolean
te_crm_command(crm_graph_t * graph, crm_action_t * action)
{
    char *counter = NULL;
    xmlNode *cmd = NULL;
    gboolean is_local = FALSE;

    const char *id = NULL;
    const char *task = NULL;
    const char *value = NULL;
    const char *on_node = NULL;
    const char *router_node = NULL;

    gboolean rc = TRUE;
    gboolean no_wait = FALSE;

    id = ID(action->xml);
    task = crm_element_value(action->xml, XML_LRM_ATTR_TASK);
    on_node = crm_element_value(action->xml, XML_LRM_ATTR_TARGET);
    router_node = crm_element_value(action->xml, XML_LRM_ATTR_ROUTER_NODE);

    if (!router_node) {
        router_node = on_node;
    }

    CRM_CHECK(on_node != NULL && strlen(on_node) != 0,
              crm_err("Corrupted command (id=%s) %s: no node", crm_str(id), crm_str(task));
              return FALSE);

    crm_info("Executing crm-event (%s): %s on %s%s%s",
             crm_str(id), crm_str(task), on_node,
             is_local ? " (local)" : "", no_wait ? " - no waiting" : "");

    if (safe_str_eq(router_node, fsa_our_uname)) {
        is_local = TRUE;
    }

    value = crm_meta_value(action->params, XML_ATTR_TE_NOWAIT);
    if (crm_is_true(value)) {
        no_wait = TRUE;
    }

    if (is_local && safe_str_eq(task, CRM_OP_SHUTDOWN)) {
        /* defer until everything else completes */
        crm_info("crm-event (%s) is a local shutdown", crm_str(id));
        graph->completion_action = tg_shutdown;
        graph->abort_reason = "local shutdown";
        te_action_confirmed(action);
        update_graph(graph, action);
        trigger_graph();
        return TRUE;

    } else if (safe_str_eq(task, CRM_OP_SHUTDOWN)) {
        crm_node_t *peer = crm_get_peer(0, router_node);
        crm_update_peer_expected(__FUNCTION__, peer, CRMD_JOINSTATE_DOWN);
    }

    cmd = create_request(task, action->xml, router_node, CRM_SYSTEM_CRMD, CRM_SYSTEM_TENGINE, NULL);

    counter =
        generate_transition_key(transition_graph->id, action->id, get_target_rc(action), te_uuid);
    crm_xml_add(cmd, XML_ATTR_TRANSITION_KEY, counter);

    rc = send_cluster_message(crm_get_peer(0, router_node), crm_msg_crmd, cmd, TRUE);
    free(counter);
    free_xml(cmd);

    if (rc == FALSE) {
        crm_err("Action %d failed: send", action->id);
        return FALSE;

    } else if (no_wait) {
        te_action_confirmed(action);
        update_graph(graph, action);
        trigger_graph();

    } else {
        if (action->timeout <= 0) {
            crm_err("Action %d: %s on %s had an invalid timeout (%dms).  Using %dms instead",
                    action->id, task, on_node, action->timeout, graph->network_delay);
            action->timeout = graph->network_delay;
        }
        te_start_action_timer(graph, action);
    }

    return TRUE;
}

gboolean
cib_action_update(crm_action_t * action, int status, int op_rc)
{
    lrmd_event_data_t *op = NULL;
    xmlNode *state = NULL;
    xmlNode *rsc = NULL;
    xmlNode *xml_op = NULL;
    xmlNode *action_rsc = NULL;

    int rc = pcmk_ok;

    const char *name = NULL;
    const char *value = NULL;
    const char *rsc_id = NULL;
    const char *task = crm_element_value(action->xml, XML_LRM_ATTR_TASK);
    const char *target = crm_element_value(action->xml, XML_LRM_ATTR_TARGET);
    const char *task_uuid = crm_element_value(action->xml, XML_LRM_ATTR_TASK_KEY);
    const char *target_uuid = crm_element_value(action->xml, XML_LRM_ATTR_TARGET_UUID);

    int call_options = cib_quorum_override | cib_scope_local;
    int target_rc = get_target_rc(action);

    if (status == PCMK_LRM_OP_PENDING) {
        crm_debug("%s %d: Recording pending operation %s on %s",
                  crm_element_name(action->xml), action->id, task_uuid, target);
    } else {
        crm_warn("%s %d: %s on %s timed out",
                 crm_element_name(action->xml), action->id, task_uuid, target);
    }

    action_rsc = find_xml_node(action->xml, XML_CIB_TAG_RESOURCE, TRUE);
    if (action_rsc == NULL) {
        return FALSE;
    }

    rsc_id = ID(action_rsc);
    CRM_CHECK(rsc_id != NULL, crm_log_xml_err(action->xml, "Bad:action");
              return FALSE);

/*
  update the CIB

<node_state id="hadev">
      <lrm>
        <lrm_resources>
          <lrm_resource id="rsc2" last_op="start" op_code="0" target="hadev"/>
*/

    state = create_xml_node(NULL, XML_CIB_TAG_STATE);

    crm_xml_add(state, XML_ATTR_UUID, target_uuid);
    crm_xml_add(state, XML_ATTR_UNAME, target);

    rsc = create_xml_node(state, XML_CIB_TAG_LRM);
    crm_xml_add(rsc, XML_ATTR_ID, target_uuid);

    rsc = create_xml_node(rsc, XML_LRM_TAG_RESOURCES);
    rsc = create_xml_node(rsc, XML_LRM_TAG_RESOURCE);
    crm_xml_add(rsc, XML_ATTR_ID, rsc_id);

    name = XML_ATTR_TYPE;
    value = crm_element_value(action_rsc, name);
    crm_xml_add(rsc, name, value);
    name = XML_AGENT_ATTR_CLASS;
    value = crm_element_value(action_rsc, name);
    crm_xml_add(rsc, name, value);
    name = XML_AGENT_ATTR_PROVIDER;
    value = crm_element_value(action_rsc, name);
    crm_xml_add(rsc, name, value);

    op = convert_graph_action(NULL, action, status, op_rc);
    op->call_id = -1;
    op->user_data = generate_transition_key(transition_graph->id, action->id, target_rc, te_uuid);

    xml_op = create_operation_update(rsc, op, CRM_FEATURE_SET, target_rc, target, __FUNCTION__, LOG_INFO);
    lrmd_free_event(op);

    crm_trace("Updating CIB with \"%s\" (%s): %s %s on %s",
              status < 0 ? "new action" : XML_ATTR_TIMEOUT,
              crm_element_name(action->xml), crm_str(task), rsc_id, target);
    crm_log_xml_trace(xml_op, "Op");

    rc = fsa_cib_conn->cmds->update(fsa_cib_conn, XML_CIB_TAG_STATUS, state, call_options);

    crm_trace("Updating CIB with %s action %d: %s on %s (call_id=%d)",
              services_lrm_status_str(status), action->id, task_uuid, target, rc);

    fsa_register_cib_callback(rc, FALSE, NULL, cib_action_updated);
    free_xml(state);

    action->sent_update = TRUE;

    if (rc < pcmk_ok) {
        return FALSE;
    }

    return TRUE;
}

static gboolean
te_rsc_command(crm_graph_t * graph, crm_action_t * action)
{
    /* never overwrite stop actions in the CIB with
     *   anything other than completed results
     *
     * Writing pending stops makes it look like the
     *   resource is running again
     */
    xmlNode *cmd = NULL;
    xmlNode *rsc_op = NULL;

    gboolean rc = TRUE;
    gboolean no_wait = FALSE;
    gboolean is_local = FALSE;

    char *counter = NULL;
    const char *task = NULL;
    const char *value = NULL;
    const char *on_node = NULL;
    const char *router_node = NULL;
    const char *task_uuid = NULL;

    CRM_ASSERT(action != NULL);
    CRM_ASSERT(action->xml != NULL);

    action->executed = FALSE;
    on_node = crm_element_value(action->xml, XML_LRM_ATTR_TARGET);

    CRM_CHECK(on_node != NULL && strlen(on_node) != 0,
              crm_err("Corrupted command(id=%s) %s: no node", ID(action->xml), crm_str(task));
              return FALSE);

    rsc_op = action->xml;
    task = crm_element_value(rsc_op, XML_LRM_ATTR_TASK);
    task_uuid = crm_element_value(action->xml, XML_LRM_ATTR_TASK_KEY);
    router_node = crm_element_value(rsc_op, XML_LRM_ATTR_ROUTER_NODE);

    if (!router_node) {
        router_node = on_node;
    }

    counter =
        generate_transition_key(transition_graph->id, action->id, get_target_rc(action), te_uuid);
    crm_xml_add(rsc_op, XML_ATTR_TRANSITION_KEY, counter);

    if (safe_str_eq(router_node, fsa_our_uname)) {
        is_local = TRUE;
    }

    value = crm_meta_value(action->params, XML_ATTR_TE_NOWAIT);
    if (crm_is_true(value)) {
        no_wait = TRUE;
    }

    crm_notice("Initiating action %d: %s %s on %s%s%s",
               action->id, task, task_uuid, on_node,
               is_local ? " (local)" : "", no_wait ? " - no waiting" : "");

    cmd = create_request(CRM_OP_INVOKE_LRM, rsc_op, router_node,
                         CRM_SYSTEM_LRMD, CRM_SYSTEM_TENGINE, NULL);

    if (is_local) {
        /* shortcut local resource commands */
        ha_msg_input_t data = {
            .msg = cmd,
            .xml = rsc_op,
        };

        fsa_data_t msg = {
            .id = 0,
            .data = &data,
            .data_type = fsa_dt_ha_msg,
            .fsa_input = I_NULL,
            .fsa_cause = C_FSA_INTERNAL,
            .actions = A_LRM_INVOKE,
            .origin = __FUNCTION__,
        };

        do_lrm_invoke(A_LRM_INVOKE, C_FSA_INTERNAL, fsa_state, I_NULL, &msg);

    } else {
        rc = send_cluster_message(crm_get_peer(0, router_node), crm_msg_lrmd, cmd, TRUE);
    }

    free(counter);
    free_xml(cmd);

    action->executed = TRUE;

    if (rc == FALSE) {
        crm_err("Action %d failed: send", action->id);
        return FALSE;

    } else if (no_wait) {
        crm_info("Action %d confirmed - no wait", action->id);
        action->confirmed = TRUE; /* Just mark confirmed.
                                   * Don't bump the job count only to immediately decrement it
                                   */
        update_graph(transition_graph, action);
        trigger_graph();

    } else if (action->confirmed == TRUE) {
        crm_debug("Action %d: %s %s on %s(timeout %dms) was already confirmed.",
                  action->id, task, task_uuid, on_node, action->timeout);
    } else {
        if (action->timeout <= 0) {
            crm_err("Action %d: %s %s on %s had an invalid timeout (%dms).  Using %dms instead",
                    action->id, task, task_uuid, on_node, action->timeout, graph->network_delay);
            action->timeout = graph->network_delay;
        }
        te_update_job_count(action, 1);
        te_start_action_timer(graph, action);
    }

    value = crm_meta_value(action->params, XML_OP_ATTR_PENDING);
    if (crm_is_true(value)
        && safe_str_neq(task, CRMD_ACTION_CANCEL)
        && safe_str_neq(task, CRMD_ACTION_DELETE)) {
        /* write a "pending" entry to the CIB, inhibit notification */
        crm_debug("Recording pending op %s in the CIB", task_uuid);
        cib_action_update(action, PCMK_LRM_OP_PENDING, PCMK_OCF_UNKNOWN);
    }

    return TRUE;
}

struct te_peer_s
{
        char *name;
        int jobs;
        int migrate_jobs;
};

static void te_peer_free(gpointer p)
{
    struct te_peer_s *peer = p;

    free(peer->name);
    free(peer);
}

void te_reset_job_counts(void)
{
    GHashTableIter iter;
    struct te_peer_s *peer = NULL;

    if(te_targets == NULL) {
        te_targets = g_hash_table_new_full(crm_str_hash, g_str_equal, NULL, te_peer_free);
    }

    g_hash_table_iter_init(&iter, te_targets);
    while (g_hash_table_iter_next(&iter, NULL, (gpointer *) & peer)) {
        peer->jobs = 0;
        peer->migrate_jobs = 0;
    }
}

static void
te_update_job_count_on(const char *target, int offset, bool migrate)
{
    struct te_peer_s *r = NULL;

    if(target == NULL || te_targets == NULL) {
        return;
    }

    r = g_hash_table_lookup(te_targets, target);
    if(r == NULL) {
        r = calloc(1, sizeof(struct te_peer_s));
        r->name = strdup(target);
        g_hash_table_insert(te_targets, r->name, r);
    }

    r->jobs += offset;
    if(migrate) {
        r->migrate_jobs += offset;
    }
    crm_trace("jobs[%s] = %d", target, r->jobs);
}

static void
te_update_job_count(crm_action_t * action, int offset)
{
    const char *task = crm_element_value(action->xml, XML_LRM_ATTR_TASK);
    const char *target = crm_element_value(action->xml, XML_LRM_ATTR_TARGET);

    if (action->type != action_type_rsc || target == NULL) {
        /* No limit on these */
        return;
    }

    /* if we have a router node, this means the action is performing
     * on a remote node. For now, we count all action occuring on a
     * remote node against the job list on the cluster node hosting
     * the connection resources */
    target = crm_element_value(action->xml, XML_LRM_ATTR_ROUTER_NODE);

    if ((target == NULL) &&
        (safe_str_eq(task, CRMD_ACTION_MIGRATE) || safe_str_eq(task, CRMD_ACTION_MIGRATED))) {

        const char *t1 = crm_meta_value(action->params, XML_LRM_ATTR_MIGRATE_SOURCE);
        const char *t2 = crm_meta_value(action->params, XML_LRM_ATTR_MIGRATE_TARGET);

        te_update_job_count_on(t1, offset, TRUE);
        te_update_job_count_on(t2, offset, TRUE);
        return;
    } else if (target == NULL) {
        target = crm_element_value(action->xml, XML_LRM_ATTR_TARGET);
    }

    te_update_job_count_on(target, offset, FALSE);
}

static gboolean
te_should_perform_action_on(crm_graph_t * graph, crm_action_t * action, const char *target)
{
    int limit = 0;
    struct te_peer_s *r = NULL;
    const char *task = crm_element_value(action->xml, XML_LRM_ATTR_TASK);
    const char *id = crm_element_value(action->xml, XML_LRM_ATTR_TASK_KEY);

    if(target == NULL) {
        /* No limit on these */
        return TRUE;

    } else if(te_targets == NULL) {
        return FALSE;
    }

    r = g_hash_table_lookup(te_targets, target);
    limit = throttle_get_job_limit(target);

    if(r == NULL) {
        r = calloc(1, sizeof(struct te_peer_s));
        r->name = strdup(target);
        g_hash_table_insert(te_targets, r->name, r);
    }

    if(limit <= r->jobs) {
        crm_trace("Peer %s is over their job limit of %d (%d): deferring %s",
                  target, limit, r->jobs, id);
        return FALSE;

    } else if(graph->migration_limit > 0 && r->migrate_jobs >= graph->migration_limit) {
        if (safe_str_eq(task, CRMD_ACTION_MIGRATE) || safe_str_eq(task, CRMD_ACTION_MIGRATED)) {
            crm_trace("Peer %s is over their migration job limit of %d (%d): deferring %s",
                      target, graph->migration_limit, r->migrate_jobs, id);
            return FALSE;
        }
    }

    crm_trace("Peer %s has not hit their limit yet. current jobs = %d limit= %d limit", target, r->jobs, limit);

    return TRUE;
}

static gboolean
te_should_perform_action(crm_graph_t * graph, crm_action_t * action)
{
    const char *target = NULL;
    const char *task = crm_element_value(action->xml, XML_LRM_ATTR_TASK);

    if (action->type != action_type_rsc) {
        /* No limit on these */
        return TRUE;
    }

    /* if we have a router node, this means the action is performing
     * on a remote node. For now, we count all action occuring on a
     * remote node against the job list on the cluster node hosting
     * the connection resources */
    target = crm_element_value(action->xml, XML_LRM_ATTR_ROUTER_NODE);

    if ((target == NULL) &&
        (safe_str_eq(task, CRMD_ACTION_MIGRATE) || safe_str_eq(task, CRMD_ACTION_MIGRATED))) {

        target = crm_meta_value(action->params, XML_LRM_ATTR_MIGRATE_SOURCE);
        if(te_should_perform_action_on(graph, action, target) == FALSE) {
            return FALSE;
        }

        target = crm_meta_value(action->params, XML_LRM_ATTR_MIGRATE_TARGET);

    } else if (target == NULL) {
        target = crm_element_value(action->xml, XML_LRM_ATTR_TARGET);
    }

    return te_should_perform_action_on(graph, action, target);
}

void
te_action_confirmed(crm_action_t * action)
{
    const char *target = crm_element_value(action->xml, XML_LRM_ATTR_TARGET);

    if (action->confirmed == FALSE && action->type == action_type_rsc && target != NULL) {
        te_update_job_count(action, -1);
    }
    action->confirmed = TRUE;
}


crm_graph_functions_t te_graph_fns = {
    te_pseudo_action,
    te_rsc_command,
    te_crm_command,
    te_fence_node,
    te_should_perform_action,
};

void
notify_crmd(crm_graph_t * graph)
{
    const char *type = "unknown";
    enum crmd_fsa_input event = I_NULL;

    crm_debug("Processing transition completion in state %s", fsa_state2string(fsa_state));

    CRM_CHECK(graph->complete, graph->complete = TRUE);

    switch (graph->completion_action) {
        case tg_stop:
            type = "stop";
            if (fsa_state == S_TRANSITION_ENGINE) {
                event = I_TE_SUCCESS;
            }
            break;
        case tg_done:
            type = "done";
            if (fsa_state == S_TRANSITION_ENGINE) {
                event = I_TE_SUCCESS;
            }
            break;

        case tg_restart:
            type = "restart";
            if (fsa_state == S_TRANSITION_ENGINE) {
                if (too_many_st_failures() == FALSE) {
                    if (transition_timer->period_ms > 0) {
                        crm_timer_stop(transition_timer);
                        crm_timer_start(transition_timer);
                    } else {
                        event = I_PE_CALC;
                    }
                } else {
                    event = I_TE_SUCCESS;
                }

            } else if (fsa_state == S_POLICY_ENGINE) {
                register_fsa_action(A_PE_INVOKE);
            }
            break;

        case tg_shutdown:
            type = "shutdown";
            if (is_set(fsa_input_register, R_SHUTDOWN)) {
                event = I_STOP;

            } else {
                crm_err("We didn't ask to be shut down, yet our PE is telling us to.");
                event = I_TERMINATE;
            }
    }

    crm_debug("Transition %d status: %s - %s", graph->id, type, crm_str(graph->abort_reason));

    graph->abort_reason = NULL;
    graph->completion_action = tg_done;
    clear_bit(fsa_input_register, R_IN_TRANSITION);

    if (event != I_NULL) {
        register_fsa_input(C_FSA_INTERNAL, event, NULL);

    } else if (fsa_source) {
        mainloop_set_trigger(fsa_source);
    }
}
