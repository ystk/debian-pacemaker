/*
 * Copyright (C) 2009 Andrew Beekhof <andrew@beekhof.net>
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
#include <sys/wait.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/utsname.h>

#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <ctype.h>

#include <crm/crm.h>
#include <crm/msg_xml.h>
#include <crm/common/ipc.h>
#include <crm/common/ipcs.h>
#include <crm/cluster/internal.h>
#include <crm/common/mainloop.h>

#include <crm/stonith-ng.h>
#include <crm/fencing/internal.h>
#include <crm/common/xml.h>

#if SUPPORT_CIBSECRETS
#  include <crm/common/cib_secrets.h>
#endif

#include <internal.h>

GHashTable *device_list = NULL;
GHashTable *topology = NULL;
GList *cmd_list = NULL;

struct device_search_s {
    /* target of fence action */
    char *host;
    /* requested fence action */
    char *action;
    /* timeout to use if a device is queried dynamically for possible targets */
    int per_device_timeout;
    /* number of registered fencing devices at time of request */
    int replies_needed;
    /* number of device replies received so far */
    int replies_received;
    /* whether the target is eligible to perform requested action (or off) */
    bool allow_suicide;

    /* private data to pass to search callback function */
    void *user_data;
    /* function to call when all replies have been received */
    void (*callback) (GList * devices, void *user_data);
    /* devices capable of performing requested action (or off if remapping) */
    GListPtr capable;
};

static gboolean stonith_device_dispatch(gpointer user_data);
static void st_child_done(GPid pid, int rc, const char *output, gpointer user_data);
static void stonith_send_reply(xmlNode * reply, int call_options, const char *remote_peer,
                               const char *client_id);

static void search_devices_record_result(struct device_search_s *search, const char *device,
                                         gboolean can_fence);

typedef struct async_command_s {

    int id;
    int pid;
    int fd_stdout;
    int options;
    int default_timeout; /* seconds */
    int timeout; /* seconds */

    int start_delay; /* milliseconds */
    int delay_id;

    char *op;
    char *origin;
    char *client;
    char *client_name;
    char *remote_op_id;

    char *victim;
    uint32_t victim_nodeid;
    char *action;
    char *device;
    char *mode;

    GListPtr device_list;
    GListPtr device_next;

    void *internal_user_data;
    void (*done_cb) (GPid pid, int rc, const char *output, gpointer user_data);
    guint timer_sigterm;
    guint timer_sigkill;
    /*! If the operation timed out, this is the last signal
     *  we sent to the process to get it to terminate */
    int last_timeout_signo;
} async_command_t;

static xmlNode *stonith_construct_async_reply(async_command_t * cmd, const char *output,
                                              xmlNode * data, int rc);

static gboolean
is_action_required(const char *action, stonith_device_t *device)
{
    return device && device->automatic_unfencing && safe_str_eq(action, "on");
}

static int
get_action_delay_max(stonith_device_t * device, const char * action)
{
    const char *value = NULL;
    int delay_max_ms = 0;

    if (safe_str_neq(action, "off") && safe_str_neq(action, "reboot")) {
        return 0;
    }

    value = g_hash_table_lookup(device->params, STONITH_ATTR_DELAY_MAX);
    if (value) {
       delay_max_ms = crm_get_msec(value);
    }

    return delay_max_ms;
}

/*!
 * \internal
 * \brief Override STONITH timeout with pcmk_*_timeout if available
 *
 * \param[in] device           STONITH device to use
 * \param[in] action           STONITH action name
 * \param[in] default_timeout  Timeout to use if device does not have
 *                             a pcmk_*_timeout parameter for action
 *
 * \return Value of pcmk_(action)_timeout if available, otherwise default_timeout
 * \note For consistency, it would be nice if reboot/off/on timeouts could be
 *       set the same way as start/stop/monitor timeouts, i.e. with an
 *       <operation> entry in the fencing resource configuration. However that
 *       is insufficient because fencing devices may be registered directly via
 *       the STONITH register_device() API instead of going through the CIB
 *       (e.g. stonith_admin uses it for its -R option, and the LRMD uses it to
 *       ensure a device is registered when a command is issued). As device
 *       properties, pcmk_*_timeout parameters can be grabbed by stonithd when
 *       the device is registered, whether by CIB change or API call.
 */
static int
get_action_timeout(stonith_device_t * device, const char *action, int default_timeout)
{
    if (action && device && device->params) {
        char buffer[64] = { 0, };
        const char *value = NULL;

        /* If "reboot" was requested but the device does not support it,
         * we will remap to "off", so check timeout for "off" instead
         */
        if (safe_str_eq(action, "reboot")
            && is_not_set(device->flags, st_device_supports_reboot)) {
            crm_trace("%s doesn't support reboot, using timeout for off instead",
                      device->id);
            action = "off";
        }

        /* If the device config specified an action-specific timeout, use it */
        snprintf(buffer, sizeof(buffer) - 1, "pcmk_%s_timeout", action);
        value = g_hash_table_lookup(device->params, buffer);
        if (value) {
            return atoi(value);
        }
    }
    return default_timeout;
}

static void
free_async_command(async_command_t * cmd)
{
    if (!cmd) {
        return;
    }

    if (cmd->delay_id) {
        g_source_remove(cmd->delay_id);
    }

    cmd_list = g_list_remove(cmd_list, cmd);

    g_list_free_full(cmd->device_list, free);
    free(cmd->device);
    free(cmd->action);
    free(cmd->victim);
    free(cmd->remote_op_id);
    free(cmd->client);
    free(cmd->client_name);
    free(cmd->origin);
    free(cmd->mode);
    free(cmd->op);
    free(cmd);
}

static async_command_t *
create_async_command(xmlNode * msg)
{
    async_command_t *cmd = NULL;
    xmlNode *op = get_xpath_object("//@" F_STONITH_ACTION, msg, LOG_ERR);
    const char *action = crm_element_value(op, F_STONITH_ACTION);

    CRM_CHECK(action != NULL, crm_log_xml_warn(msg, "NoAction"); return NULL);

    crm_log_xml_trace(msg, "Command");
    cmd = calloc(1, sizeof(async_command_t));
    crm_element_value_int(msg, F_STONITH_CALLID, &(cmd->id));
    crm_element_value_int(msg, F_STONITH_CALLOPTS, &(cmd->options));
    crm_element_value_int(msg, F_STONITH_TIMEOUT, &(cmd->default_timeout));
    cmd->timeout = cmd->default_timeout;

    cmd->origin = crm_element_value_copy(msg, F_ORIG);
    cmd->remote_op_id = crm_element_value_copy(msg, F_STONITH_REMOTE_OP_ID);
    cmd->client = crm_element_value_copy(msg, F_STONITH_CLIENTID);
    cmd->client_name = crm_element_value_copy(msg, F_STONITH_CLIENTNAME);
    cmd->op = crm_element_value_copy(msg, F_STONITH_OPERATION);
    cmd->action = strdup(action);
    cmd->victim = crm_element_value_copy(op, F_STONITH_TARGET);
    cmd->mode = crm_element_value_copy(op, F_STONITH_MODE);
    cmd->device = crm_element_value_copy(op, F_STONITH_DEVICE);

    CRM_CHECK(cmd->op != NULL, crm_log_xml_warn(msg, "NoOp"); free_async_command(cmd); return NULL);
    CRM_CHECK(cmd->client != NULL, crm_log_xml_warn(msg, "NoClient"));

    cmd->done_cb = st_child_done;
    cmd_list = g_list_append(cmd_list, cmd);
    return cmd;
}

static gboolean
stonith_device_execute(stonith_device_t * device)
{
    int exec_rc = 0;
    const char *action_str = NULL;
    async_command_t *cmd = NULL;
    stonith_action_t *action = NULL;

    CRM_CHECK(device != NULL, return FALSE);

    if (device->active_pid) {
        crm_trace("%s is still active with pid %u", device->id, device->active_pid);
        return TRUE;
    }

    if (device->pending_ops) {
        GList *first = device->pending_ops;

        cmd = first->data;
        if (cmd && cmd->delay_id) {
            crm_trace
                ("Operation %s%s%s on %s was asked to run too early, waiting for start_delay timeout of %dms",
                 cmd->action, cmd->victim ? " for node " : "", cmd->victim ? cmd->victim : "",
                 device->id, cmd->start_delay);
            return TRUE;
        }

        device->pending_ops = g_list_remove_link(device->pending_ops, first);
        g_list_free_1(first);
    }

    if (cmd == NULL) {
        crm_trace("Nothing further to do for %s", device->id);
        return TRUE;
    }

    if(safe_str_eq(device->agent, STONITH_WATCHDOG_AGENT)) {
        if(safe_str_eq(cmd->action, "reboot")) {
            pcmk_panic(__FUNCTION__);
            return TRUE;

        } else if(safe_str_eq(cmd->action, "off")) {
            pcmk_panic(__FUNCTION__);
            return TRUE;

        } else {
            crm_info("Faking success for %s watchdog operation", cmd->action);
            cmd->done_cb(0, 0, NULL, cmd);
            return TRUE;
        }
    }

#if SUPPORT_CIBSECRETS
    if (replace_secret_params(device->id, device->params) < 0) {
        /* replacing secrets failed! */
        if (safe_str_eq(cmd->action,"stop")) {
            /* don't fail on stop! */
            crm_info("proceeding with the stop operation for %s", device->id);

        } else {
            crm_err("failed to get secrets for %s, "
                    "considering resource not configured", device->id);
            exec_rc = PCMK_OCF_NOT_CONFIGURED;
            cmd->done_cb(0, exec_rc, NULL, cmd);
            return TRUE;
        }
    }
#endif

    action_str = cmd->action;
    if (safe_str_eq(cmd->action, "reboot") && is_not_set(device->flags, st_device_supports_reboot)) {
        crm_warn("Agent '%s' does not advertise support for 'reboot', performing 'off' action instead", device->agent);
        action_str = "off";
    }

    action = stonith_action_create(device->agent,
                                   action_str,
                                   cmd->victim,
                                   cmd->victim_nodeid,
                                   cmd->timeout, device->params, device->aliases);

    /* for async exec, exec_rc is pid if positive and error code if negative/zero */
    exec_rc = stonith_action_execute_async(action, (void *)cmd, cmd->done_cb);

    if (exec_rc > 0) {
        crm_debug("Operation %s%s%s on %s now running with pid=%d, timeout=%ds",
                  cmd->action, cmd->victim ? " for node " : "", cmd->victim ? cmd->victim : "",
                  device->id, exec_rc, cmd->timeout);
        device->active_pid = exec_rc;

    } else {
        crm_warn("Operation %s%s%s on %s failed: %s (%d)",
                 cmd->action, cmd->victim ? " for node " : "", cmd->victim ? cmd->victim : "",
                 device->id, pcmk_strerror(exec_rc), exec_rc);
        cmd->done_cb(0, exec_rc, NULL, cmd);
    }
    return TRUE;
}

static gboolean
stonith_device_dispatch(gpointer user_data)
{
    return stonith_device_execute(user_data);
}

static gboolean
start_delay_helper(gpointer data)
{
    async_command_t *cmd = data;
    stonith_device_t *device = NULL;

    cmd->delay_id = 0;
    device = cmd->device ? g_hash_table_lookup(device_list, cmd->device) : NULL;

    if (device) {
        mainloop_set_trigger(device->work);
    }

    return FALSE;
}

static void
schedule_stonith_command(async_command_t * cmd, stonith_device_t * device)
{
    int delay_max = 0;

    CRM_CHECK(cmd != NULL, return);
    CRM_CHECK(device != NULL, return);

    if (cmd->device) {
        free(cmd->device);
    }

    if (device->include_nodeid && cmd->victim) {
        crm_node_t *node = crm_get_peer(0, cmd->victim);

        cmd->victim_nodeid = node->id;
    }

    cmd->device = strdup(device->id);
    cmd->timeout = get_action_timeout(device, cmd->action, cmd->default_timeout);

    if (cmd->remote_op_id) {
        crm_debug("Scheduling %s on %s for remote peer %s with op id (%s) (timeout=%ds)",
                  cmd->action, device->id, cmd->origin, cmd->remote_op_id, cmd->timeout);
    } else {
        crm_debug("Scheduling %s on %s for %s (timeout=%ds)",
                  cmd->action, device->id, cmd->client, cmd->timeout);
    }

    device->pending_ops = g_list_append(device->pending_ops, cmd);
    mainloop_set_trigger(device->work);

    delay_max = get_action_delay_max(device, cmd->action);
    if (delay_max > 0) {
        cmd->start_delay = rand() % delay_max;
        crm_notice("Delaying %s on %s for %lldms (timeout=%ds)",
                    cmd->action, device->id, cmd->start_delay, cmd->timeout);
        cmd->delay_id = g_timeout_add(cmd->start_delay, start_delay_helper, cmd);
    }
}

void
free_device(gpointer data)
{
    GListPtr gIter = NULL;
    stonith_device_t *device = data;

    g_hash_table_destroy(device->params);
    g_hash_table_destroy(device->aliases);

    for (gIter = device->pending_ops; gIter != NULL; gIter = gIter->next) {
        async_command_t *cmd = gIter->data;

        crm_warn("Removal of device '%s' purged operation %s", device->id, cmd->action);
        cmd->done_cb(0, -ENODEV, NULL, cmd);
        free_async_command(cmd);
    }
    g_list_free(device->pending_ops);

    g_list_free_full(device->targets, free);

    mainloop_destroy_trigger(device->work);

    free_xml(device->agent_metadata);
    free(device->namespace);
    free(device->on_target_actions);
    free(device->agent);
    free(device->id);
    free(device);
}

static GHashTable *
build_port_aliases(const char *hostmap, GListPtr * targets)
{
    char *name = NULL;
    int last = 0, lpc = 0, max = 0, added = 0;
    GHashTable *aliases =
        g_hash_table_new_full(crm_strcase_hash, crm_strcase_equal, g_hash_destroy_str, g_hash_destroy_str);

    if (hostmap == NULL) {
        return aliases;
    }

    max = strlen(hostmap);
    for (; lpc <= max; lpc++) {
        switch (hostmap[lpc]) {
                /* Assignment chars */
            case '=':
            case ':':
                if (lpc > last) {
                    free(name);
                    name = calloc(1, 1 + lpc - last);
                    memcpy(name, hostmap + last, lpc - last);
                }
                last = lpc + 1;
                break;

                /* Delimeter chars */
                /* case ',': Potentially used to specify multiple ports */
            case 0:
            case ';':
            case ' ':
            case '\t':
                if (name) {
                    char *value = NULL;

                    value = calloc(1, 1 + lpc - last);
                    memcpy(value, hostmap + last, lpc - last);

                    crm_debug("Adding alias '%s'='%s'", name, value);
                    g_hash_table_replace(aliases, name, value);
                    if (targets) {
                        *targets = g_list_append(*targets, strdup(value));
                    }
                    value = NULL;
                    name = NULL;
                    added++;

                } else if (lpc > last) {
                    crm_debug("Parse error at offset %d near '%s'", lpc - last, hostmap + last);
                }

                last = lpc + 1;
                break;
        }

        if (hostmap[lpc] == 0) {
            break;
        }
    }

    if (added == 0) {
        crm_info("No host mappings detected in '%s'", hostmap);
    }

    free(name);
    return aliases;
}

static void
parse_host_line(const char *line, int max, GListPtr * output)
{
    int lpc = 0;
    int last = 0;

    if (max <= 0) {
        return;
    }

    /* Check for any complaints about additional parameters that the device doesn't understand */
    if (strstr(line, "invalid") || strstr(line, "variable")) {
        crm_debug("Skipping: %s", line);
        return;
    }

    crm_trace("Processing %d bytes: [%s]", max, line);
    /* Skip initial whitespace */
    for (lpc = 0; lpc <= max && isspace(line[lpc]); lpc++) {
        last = lpc + 1;
    }

    /* Now the actual content */
    for (lpc = 0; lpc <= max; lpc++) {
        gboolean a_space = isspace(line[lpc]);

        if (a_space && lpc < max && isspace(line[lpc + 1])) {
            /* fast-forward to the end of the spaces */

        } else if (a_space || line[lpc] == ',' || line[lpc] == ';' || line[lpc] == 0) {
            int rc = 1;
            char *entry = NULL;

            if (lpc != last) {
                entry = calloc(1, 1 + lpc - last);
                rc = sscanf(line + last, "%[a-zA-Z0-9_-.]", entry);
            }

            if (entry == NULL) {
                /* Skip */
            } else if (rc != 1) {
                crm_warn("Could not parse (%d %d): %s", last, lpc, line + last);
            } else if (safe_str_neq(entry, "on") && safe_str_neq(entry, "off")) {
                crm_trace("Adding '%s'", entry);
                *output = g_list_append(*output, entry);
                entry = NULL;
            }

            free(entry);
            last = lpc + 1;
        }
    }
}

static GListPtr
parse_host_list(const char *hosts)
{
    int lpc = 0;
    int max = 0;
    int last = 0;
    GListPtr output = NULL;

    if (hosts == NULL) {
        return output;
    }

    max = strlen(hosts);
    for (lpc = 0; lpc <= max; lpc++) {
        if (hosts[lpc] == '\n' || hosts[lpc] == 0) {
            char *line = NULL;
            int len = lpc - last;

            if(len > 1) {
                line = malloc(1 + len);
            }

            if(line) {
                snprintf(line, 1 + len, "%s", hosts + last);
                line[len] = 0; /* Because it might be '\n' */
                parse_host_line(line, len, &output);
                free(line);
            }

            last = lpc + 1;
        }
    }

    crm_trace("Parsed %d entries from '%s'", g_list_length(output), hosts);
    return output;
}

GHashTable *metadata_cache = NULL;

static xmlNode *
get_agent_metadata(const char *agent)
{
    xmlNode *xml = NULL;
    char *buffer = NULL;

    if(metadata_cache == NULL) {
        metadata_cache = g_hash_table_new_full(
            crm_str_hash, g_str_equal, g_hash_destroy_str, g_hash_destroy_str);
    }

    buffer = g_hash_table_lookup(metadata_cache, agent);
    if(safe_str_eq(agent, STONITH_WATCHDOG_AGENT)) {
        return NULL;

    } else if(buffer == NULL) {
        stonith_t *st = stonith_api_new();
        int rc = st->cmds->metadata(st, st_opt_sync_call, agent, NULL, &buffer, 10);

        stonith_api_delete(st);
        if (rc || !buffer) {
            crm_err("Could not retrieve metadata for fencing agent %s", agent);
            return NULL;
        }
        g_hash_table_replace(metadata_cache, strdup(agent), buffer);
    }

    xml = string2xml(buffer);

    return xml;
}

static gboolean
is_nodeid_required(xmlNode * xml)
{
    xmlXPathObjectPtr xpath = NULL;

    if (stand_alone) {
        return FALSE;
    }

    if (!xml) {
        return FALSE;
    }

    xpath = xpath_search(xml, "//parameter[@name='nodeid']");
    if (numXpathResults(xpath)  <= 0) {
        freeXpathObject(xpath);
        return FALSE;
    }

    freeXpathObject(xpath);
    return TRUE;
}

static char *
add_action(char *actions, const char *action)
{
    static size_t len = 256;
    int offset = 0;

    if (actions == NULL) {
        actions = calloc(1, len);
    } else {
        offset = strlen(actions);
    }

    if (offset > 0) {
        offset += snprintf(actions+offset, len-offset, " ");
    }
    offset += snprintf(actions+offset, len-offset, "%s", action);

    return actions;
}

static void
read_action_metadata(stonith_device_t *device)
{
    xmlXPathObjectPtr xpath = NULL;
    int max = 0;
    int lpc = 0;

    if (device->agent_metadata == NULL) {
        return;
    }

    xpath = xpath_search(device->agent_metadata, "//action");
    max = numXpathResults(xpath);

    if (max <= 0) {
        freeXpathObject(xpath);
        return;
    }

    for (lpc = 0; lpc < max; lpc++) {
        const char *on_target = NULL;
        const char *action = NULL;
        xmlNode *match = getXpathResult(xpath, lpc);

        CRM_LOG_ASSERT(match != NULL);
        if(match == NULL) { continue; };

        on_target = crm_element_value(match, "on_target");
        action = crm_element_value(match, "name");

        if(safe_str_eq(action, "list")) {
            set_bit(device->flags, st_device_supports_list);
        } else if(safe_str_eq(action, "status")) {
            set_bit(device->flags, st_device_supports_status);
        } else if(safe_str_eq(action, "reboot")) {
            set_bit(device->flags, st_device_supports_reboot);
        } else if (safe_str_eq(action, "on")) {
            /* "automatic" means the cluster will unfence node when it joins */
            const char *automatic = crm_element_value(match, "automatic");

            /* "required" is a deprecated synonym for "automatic" */
            const char *required = crm_element_value(match, "required");

            if (crm_is_true(automatic) || crm_is_true(required)) {
                device->automatic_unfencing = TRUE;
            }
        }

        if (action && crm_is_true(on_target)) {
            device->on_target_actions = add_action(device->on_target_actions, action);
        }
    }

    freeXpathObject(xpath);
}

static stonith_device_t *
build_device_from_xml(xmlNode * msg)
{
    const char *value = NULL;
    xmlNode *dev = get_xpath_object("//" F_STONITH_DEVICE, msg, LOG_ERR);
    stonith_device_t *device = NULL;

    device = calloc(1, sizeof(stonith_device_t));
    device->id = crm_element_value_copy(dev, XML_ATTR_ID);
    device->agent = crm_element_value_copy(dev, "agent");
    device->namespace = crm_element_value_copy(dev, "namespace");
    device->params = xml2list(dev);

    value = g_hash_table_lookup(device->params, STONITH_ATTR_HOSTLIST);
    if (value) {
        device->targets = parse_host_list(value);
    }

    value = g_hash_table_lookup(device->params, STONITH_ATTR_HOSTMAP);
    device->aliases = build_port_aliases(value, &(device->targets));

    device->agent_metadata = get_agent_metadata(device->agent);
    read_action_metadata(device);

    value = g_hash_table_lookup(device->params, "nodeid");
    if (!value) {
        device->include_nodeid = is_nodeid_required(device->agent_metadata);
    }

    value = crm_element_value(dev, "rsc_provides");
    if (safe_str_eq(value, "unfencing")) {
        device->automatic_unfencing = TRUE;
    }

    if (is_action_required("on", device)) {
        crm_info("The fencing device '%s' requires unfencing", device->id);
    }

    if (device->on_target_actions) {
        crm_info("The fencing device '%s' requires actions (%s) to be executed on the target node",
                 device->id, device->on_target_actions);
    }

    device->work = mainloop_add_trigger(G_PRIORITY_HIGH, stonith_device_dispatch, device);
    /* TODO: Hook up priority */

    return device;
}

static const char *
target_list_type(stonith_device_t * dev)
{
    const char *check_type = NULL;

    check_type = g_hash_table_lookup(dev->params, STONITH_ATTR_HOSTCHECK);

    if (check_type == NULL) {

        if (g_hash_table_lookup(dev->params, STONITH_ATTR_HOSTLIST)) {
            check_type = "static-list";
        } else if (g_hash_table_lookup(dev->params, STONITH_ATTR_HOSTMAP)) {
            check_type = "static-list";
        } else if(is_set(dev->flags, st_device_supports_list)){
            check_type = "dynamic-list";
        } else if(is_set(dev->flags, st_device_supports_status)){
            check_type = "status";
        } else {
            check_type = "none";
        }
    }

    return check_type;
}

void
schedule_internal_command(const char *origin,
                          stonith_device_t * device,
                          const char *action,
                          const char *victim,
                          int timeout,
                          void *internal_user_data,
                          void (*done_cb) (GPid pid, int rc, const char *output,
                                           gpointer user_data))
{
    async_command_t *cmd = NULL;

    cmd = calloc(1, sizeof(async_command_t));

    cmd->id = -1;
    cmd->default_timeout = timeout ? timeout : 60;
    cmd->timeout = cmd->default_timeout;
    cmd->action = strdup(action);
    cmd->victim = victim ? strdup(victim) : NULL;
    cmd->device = strdup(device->id);
    cmd->origin = strdup(origin);
    cmd->client = strdup(crm_system_name);
    cmd->client_name = strdup(crm_system_name);

    cmd->internal_user_data = internal_user_data;
    cmd->done_cb = done_cb; /* cmd, not internal_user_data, is passed to 'done_cb' as the userdata */

    schedule_stonith_command(cmd, device);
}

gboolean
string_in_list(GListPtr list, const char *item)
{
    int lpc = 0;
    int max = g_list_length(list);

    for (lpc = 0; lpc < max; lpc++) {
        const char *value = g_list_nth_data(list, lpc);

        if (safe_str_eq(item, value)) {
            return TRUE;
        } else {
            crm_trace("%d: '%s' != '%s'", lpc, item, value);
        }
    }
    return FALSE;
}

static void
status_search_cb(GPid pid, int rc, const char *output, gpointer user_data)
{
    async_command_t *cmd = user_data;
    struct device_search_s *search = cmd->internal_user_data;
    stonith_device_t *dev = cmd->device ? g_hash_table_lookup(device_list, cmd->device) : NULL;
    gboolean can = FALSE;

    free_async_command(cmd);

    if (!dev) {
        search_devices_record_result(search, NULL, FALSE);
        return;
    }

    dev->active_pid = 0;
    mainloop_set_trigger(dev->work);

    if (rc == 1 /* unknown */ ) {
        crm_trace("Host %s is not known by %s", search->host, dev->id);

    } else if (rc == 0 /* active */  || rc == 2 /* inactive */ ) {
        crm_trace("Host %s is known by %s", search->host, dev->id);
        can = TRUE;

    } else {
        crm_notice("Unknown result when testing if %s can fence %s: rc=%d", dev->id, search->host,
                   rc);
    }
    search_devices_record_result(search, dev->id, can);
}

static void
dynamic_list_search_cb(GPid pid, int rc, const char *output, gpointer user_data)
{
    async_command_t *cmd = user_data;
    struct device_search_s *search = cmd->internal_user_data;
    stonith_device_t *dev = cmd->device ? g_hash_table_lookup(device_list, cmd->device) : NULL;
    gboolean can_fence = FALSE;

    free_async_command(cmd);

    /* Host/alias must be in the list output to be eligible to be fenced
     *
     * Will cause problems if down'd nodes aren't listed or (for virtual nodes)
     *  if the guest is still listed despite being moved to another machine
     */
    if (!dev) {
        search_devices_record_result(search, NULL, FALSE);
        return;
    }

    dev->active_pid = 0;
    mainloop_set_trigger(dev->work);

    /* If we successfully got the targets earlier, don't disable. */
    if (rc != 0 && !dev->targets) {
        crm_notice("Disabling port list queries for %s (%d): %s", dev->id, rc, output);
        /* Fall back to status */
        g_hash_table_replace(dev->params, strdup(STONITH_ATTR_HOSTCHECK), strdup("status"));

        g_list_free_full(dev->targets, free);
        dev->targets = NULL;
    } else if (!rc) {
        crm_info("Refreshing port list for %s", dev->id);
        g_list_free_full(dev->targets, free);
        dev->targets = parse_host_list(output);
        dev->targets_age = time(NULL);
    }

    if (dev->targets) {
        const char *alias = g_hash_table_lookup(dev->aliases, search->host);

        if (!alias) {
            alias = search->host;
        }
        if (string_in_list(dev->targets, alias)) {
            can_fence = TRUE;
        }
    }
    search_devices_record_result(search, dev->id, can_fence);
}

/*!
 * \internal
 * \brief Checks to see if an identical device already exists in the device_list
 */
static stonith_device_t *
device_has_duplicate(stonith_device_t * device)
{
    char *key = NULL;
    char *value = NULL;
    GHashTableIter gIter;
    stonith_device_t *dup = g_hash_table_lookup(device_list, device->id);

    if (!dup) {
        crm_trace("No match for %s", device->id);
        return NULL;

    } else if (safe_str_neq(dup->agent, device->agent)) {
        crm_trace("Different agent: %s != %s", dup->agent, device->agent);
        return NULL;
    }

    /* Use calculate_operation_digest() here? */
    g_hash_table_iter_init(&gIter, device->params);
    while (g_hash_table_iter_next(&gIter, (void **)&key, (void **)&value)) {

        if(strstr(key, "CRM_meta") == key) {
            continue;
        } else if(strcmp(key, "crm_feature_set") == 0) {
            continue;
        } else {
            char *other_value = g_hash_table_lookup(dup->params, key);

            if (!other_value || safe_str_neq(other_value, value)) {
                crm_trace("Different value for %s: %s != %s", key, other_value, value);
                return NULL;
            }
        }
    }

    crm_trace("Match");
    return dup;
}

int
stonith_device_register(xmlNode * msg, const char **desc, gboolean from_cib)
{
    stonith_device_t *dup = NULL;
    stonith_device_t *device = build_device_from_xml(msg);

    dup = device_has_duplicate(device);
    if (dup) {
        crm_debug("Device '%s' already existed in device list (%d active devices)", device->id,
                   g_hash_table_size(device_list));
        free_device(device);
        device = dup;

    } else {
        stonith_device_t *old = g_hash_table_lookup(device_list, device->id);

        if (from_cib && old && old->api_registered) {
            /* If the cib is writing over an entry that is shared with a stonith client,
             * copy any pending ops that currently exist on the old entry to the new one.
             * Otherwise the pending ops will be reported as failures
             */
            crm_info("Overwriting an existing entry for %s from the cib", device->id);
            device->pending_ops = old->pending_ops;
            device->api_registered = TRUE;
            old->pending_ops = NULL;
            if (device->pending_ops) {
                mainloop_set_trigger(device->work);
            }
        }
        g_hash_table_replace(device_list, device->id, device);

        crm_notice("Added '%s' to the device list (%d active devices)", device->id,
                   g_hash_table_size(device_list));
    }
    if (desc) {
        *desc = device->id;
    }

    if (from_cib) {
        device->cib_registered = TRUE;
    } else {
        device->api_registered = TRUE;
    }

    return pcmk_ok;
}

int
stonith_device_remove(const char *id, gboolean from_cib)
{
    stonith_device_t *device = g_hash_table_lookup(device_list, id);

    if (!device) {
        crm_info("Device '%s' not found (%d active devices)", id, g_hash_table_size(device_list));
        return pcmk_ok;
    }

    if (from_cib) {
        device->cib_registered = FALSE;
    } else {
        device->verified = FALSE;
        device->api_registered = FALSE;
    }

    if (!device->cib_registered && !device->api_registered) {
        g_hash_table_remove(device_list, id);
        crm_info("Removed '%s' from the device list (%d active devices)",
                 id, g_hash_table_size(device_list));
    }
    return pcmk_ok;
}

/*!
 * \internal
 * \brief Return the number of stonith levels registered for a node
 *
 * \param[in] tp  Node's topology table entry
 *
 * \return Number of non-NULL levels in topology entry
 * \note This function is used only for log messages.
 */
static int
count_active_levels(stonith_topology_t * tp)
{
    int lpc = 0;
    int count = 0;

    for (lpc = 0; lpc < ST_LEVEL_MAX; lpc++) {
        if (tp->levels[lpc] != NULL) {
            count++;
        }
    }
    return count;
}

void
free_topology_entry(gpointer data)
{
    stonith_topology_t *tp = data;

    int lpc = 0;

    for (lpc = 0; lpc < ST_LEVEL_MAX; lpc++) {
        if (tp->levels[lpc] != NULL) {
            g_list_free_full(tp->levels[lpc], free);
        }
    }
    free(tp->target);
    free(tp->target_value);
    free(tp->target_pattern);
    free(tp->target_attribute);
    free(tp);
}

char *stonith_level_key(xmlNode *level, int mode)
{
    if(mode == -1) {
        mode = stonith_level_kind(level);
    }

    switch(mode) {
        case 0:
            return crm_element_value_copy(level, XML_ATTR_STONITH_TARGET);
        case 1:
            return crm_element_value_copy(level, XML_ATTR_STONITH_TARGET_PATTERN);
        case 2:
            {
                const char *name = crm_element_value(level, XML_ATTR_STONITH_TARGET_ATTRIBUTE);
                const char *value = crm_element_value(level, XML_ATTR_STONITH_TARGET_VALUE);

                if(name && value) {
                    return crm_strdup_printf("%s=%s", name, value);
                }
            }
        default:
            return crm_strdup_printf("Unknown-%d-%s", mode, ID(level));
    }
}

int stonith_level_kind(xmlNode * level)
{
    int mode = 0;
    const char *target = crm_element_value(level, XML_ATTR_STONITH_TARGET);

    if(target == NULL) {
        mode++;
        target = crm_element_value(level, XML_ATTR_STONITH_TARGET_PATTERN);
    }

    if(stand_alone == FALSE && target == NULL) {

        mode++;

        if(crm_element_value(level, XML_ATTR_STONITH_TARGET_ATTRIBUTE) == NULL) {
            mode++;

        } else if(crm_element_value(level, XML_ATTR_STONITH_TARGET_VALUE) == NULL) {
            mode++;
        }
    }

    return mode;
}

static stonith_key_value_t *
parse_device_list(const char *devices)
{
    int lpc = 0;
    int max = 0;
    int last = 0;
    stonith_key_value_t *output = NULL;

    if (devices == NULL) {
        return output;
    }

    max = strlen(devices);
    for (lpc = 0; lpc <= max; lpc++) {
        if (devices[lpc] == ',' || devices[lpc] == 0) {
            char *line = NULL;

            line = calloc(1, 2 + lpc - last);
            snprintf(line, 1 + lpc - last, "%s", devices + last);
            output = stonith_key_value_add(output, NULL, line);
            free(line);

            last = lpc + 1;
        }
    }

    return output;
}

/*!
 * \internal
 * \brief Register a STONITH level for a target
 *
 * Given an XML request specifying the target name, level index, and device IDs
 * for the level, this will create an entry for the target in the global topology
 * table if one does not already exist, then append the specified device IDs to
 * the entry's device list for the specified level.
 *
 * \param[in]  msg   XML request for STONITH level registration
 * \param[out] desc  If not NULL, will be set to string representation ("TARGET[LEVEL]")
 *
 * \return pcmk_ok on success, -EINVAL if XML does not specify valid level index
 */
int
stonith_level_register(xmlNode *msg, char **desc)
{
    int id = 0;
    xmlNode *level;
    int mode;
    char *target;

    stonith_topology_t *tp;
    stonith_key_value_t *dIter = NULL;
    stonith_key_value_t *devices = NULL;

    /* Allow the XML here to point to the level tag directly, or wrapped in
     * another tag. If directly, don't search by xpath, because it might give
     * multiple hits (e.g. if the XML is the CIB).
     */
    if (safe_str_eq(TYPE(msg), XML_TAG_FENCING_LEVEL)) {
        level = msg;
    } else {
        level = get_xpath_object("//" XML_TAG_FENCING_LEVEL, msg, LOG_ERR);
    }
    CRM_CHECK(level != NULL, return -EINVAL);

    mode = stonith_level_kind(level);
    target = stonith_level_key(level, mode);
    crm_element_value_int(level, XML_ATTR_STONITH_INDEX, &id);

    if (desc) {
        *desc = crm_strdup_printf("%s[%d]", target, id);
    }

    /* Sanity-check arguments */
    if (mode >= 3 || (id <= 0) || (id >= ST_LEVEL_MAX)) {
        crm_trace("Could not add %s[%d] (%d) to the topology (%d active entries)", target, id, mode, g_hash_table_size(topology));
        free(target);
        crm_log_xml_err(level, "Bad topology");
        return -EINVAL;
    }

    /* Find or create topology table entry */
    tp = g_hash_table_lookup(topology, target);
    if (tp == NULL) {
        tp = calloc(1, sizeof(stonith_topology_t));
        tp->kind = mode;
        tp->target = target;
        tp->target_value = crm_element_value_copy(level, XML_ATTR_STONITH_TARGET_VALUE);
        tp->target_pattern = crm_element_value_copy(level, XML_ATTR_STONITH_TARGET_PATTERN);
        tp->target_attribute = crm_element_value_copy(level, XML_ATTR_STONITH_TARGET_ATTRIBUTE);

        g_hash_table_replace(topology, tp->target, tp);
        crm_trace("Added %s (%d) to the topology (%d active entries)",
                  target, mode, g_hash_table_size(topology));
    } else {
        free(target);
    }

    if (tp->levels[id] != NULL) {
        crm_info("Adding to the existing %s[%d] topology entry",
                 tp->target, id);
    }

    devices = parse_device_list(crm_element_value(level, XML_ATTR_STONITH_DEVICES));
    for (dIter = devices; dIter; dIter = dIter->next) {
        const char *device = dIter->value;

        crm_trace("Adding device '%s' for %s[%d]", device, tp->target, id);
        tp->levels[id] = g_list_append(tp->levels[id], strdup(device));
    }
    stonith_key_value_freeall(devices, 1, 1);

    crm_info("Target %s has %d active fencing levels",
             tp->target, count_active_levels(tp));
    return pcmk_ok;
}

int
stonith_level_remove(xmlNode *msg, char **desc)
{
    int id = 0;
    stonith_topology_t *tp;
    char *target;

    /* Unlike additions, removal requests should always have one level tag */
    xmlNode *level = get_xpath_object("//" XML_TAG_FENCING_LEVEL, msg, LOG_ERR);

    CRM_CHECK(level != NULL, return -EINVAL);

    target = stonith_level_key(level, -1);
    crm_element_value_int(level, XML_ATTR_STONITH_INDEX, &id);
    if (desc) {
        *desc = crm_strdup_printf("%s[%d]", target, id);
    }

    /* Sanity-check arguments */
    if (id >= ST_LEVEL_MAX) {
        free(target);
        return -EINVAL;
    }

    tp = g_hash_table_lookup(topology, target);
    if (tp == NULL) {
        crm_info("Topology for %s not found (%d active entries)",
                 target, g_hash_table_size(topology));

    } else if (id == 0 && g_hash_table_remove(topology, target)) {
        crm_info("Removed all %s related entries from the topology (%d active entries)",
                 target, g_hash_table_size(topology));

    } else if (id > 0 && tp->levels[id] != NULL) {
        g_list_free_full(tp->levels[id], free);
        tp->levels[id] = NULL;

        crm_info("Removed level '%d' from topology for %s (%d active levels remaining)",
                 id, target, count_active_levels(tp));
    }

    free(target);
    return pcmk_ok;
}

static int
stonith_device_action(xmlNode * msg, char **output)
{
    int rc = pcmk_ok;
    xmlNode *dev = get_xpath_object("//" F_STONITH_DEVICE, msg, LOG_ERR);
    const char *id = crm_element_value(dev, F_STONITH_DEVICE);

    async_command_t *cmd = NULL;
    stonith_device_t *device = NULL;

    if (id) {
        crm_trace("Looking for '%s'", id);
        device = g_hash_table_lookup(device_list, id);
    }

    if (device && device->api_registered == FALSE) {
        rc = -ENODEV;

    } else if (device) {
        cmd = create_async_command(msg);
        if (cmd == NULL) {
            return -EPROTO;
        }

        schedule_stonith_command(cmd, device);
        rc = -EINPROGRESS;

    } else {
        crm_info("Device %s not found", id ? id : "<none>");
        rc = -ENODEV;
    }
    return rc;
}

static void
search_devices_record_result(struct device_search_s *search, const char *device, gboolean can_fence)
{
    search->replies_received++;

    if (can_fence && device) {
        search->capable = g_list_append(search->capable, strdup(device));
    }

    if (search->replies_needed == search->replies_received) {

        crm_debug("Finished Search. %d devices can perform action (%s) on node %s",
                  g_list_length(search->capable),
                  search->action ? search->action : "<unknown>",
                  search->host ? search->host : "<anyone>");

        search->callback(search->capable, search->user_data);
        free(search->host);
        free(search->action);
        free(search);
    }
}

/*
 * \internal
 * \brief Check whether the local host is allowed to execute a fencing action
 *
 * \param[in] device         Fence device to check
 * \param[in] action         Fence action to check
 * \param[in] target         Hostname of fence target
 * \param[in] allow_suicide  Whether self-fencing is allowed for this operation
 *
 * \return TRUE if local host is allowed to execute action, FALSE otherwise
 */
static gboolean
localhost_is_eligible(const stonith_device_t *device, const char *action,
                      const char *target, gboolean allow_suicide)
{
    gboolean localhost_is_target = safe_str_eq(target, stonith_our_uname);

    if (device && action && device->on_target_actions
        && strstr(device->on_target_actions, action)) {
        if (!localhost_is_target) {
            crm_trace("%s operation with %s can only be executed for localhost not %s",
                      action, device->id, target);
            return FALSE;
        }

    } else if (localhost_is_target && !allow_suicide) {
        crm_trace("%s operation does not support self-fencing", action);
        return FALSE;
    }
    return TRUE;
}

static void
can_fence_host_with_device(stonith_device_t * dev, struct device_search_s *search)
{
    gboolean can = FALSE;
    const char *check_type = NULL;
    const char *host = search->host;
    const char *alias = NULL;

    CRM_LOG_ASSERT(dev != NULL);

    if (dev == NULL) {
        goto search_report_results;
    } else if (host == NULL) {
        can = TRUE;
        goto search_report_results;
    }

    /* Short-circuit query if this host is not allowed to perform the action */
    if (safe_str_eq(search->action, "reboot")) {
        /* A "reboot" *might* get remapped to "off" then "on", so short-circuit
         * only if all three are disallowed. If only one or two are disallowed,
         * we'll report that with the results. We never allow suicide for
         * remapped "on" operations because the host is off at that point.
         */
        if (!localhost_is_eligible(dev, "reboot", host, search->allow_suicide)
            && !localhost_is_eligible(dev, "off", host, search->allow_suicide)
            && !localhost_is_eligible(dev, "on", host, FALSE)) {
            goto search_report_results;
        }
    } else if (!localhost_is_eligible(dev, search->action, host,
                                      search->allow_suicide)) {
        goto search_report_results;
    }

    alias = g_hash_table_lookup(dev->aliases, host);
    if (alias == NULL) {
        alias = host;
    }

    check_type = target_list_type(dev);

    if (safe_str_eq(check_type, "none")) {
        can = TRUE;

    } else if (safe_str_eq(check_type, "static-list")) {

        /* Presence in the hostmap is sufficient
         * Only use if all hosts on which the device can be active can always fence all listed hosts
         */

        if (string_in_list(dev->targets, host)) {
            can = TRUE;
        } else if (g_hash_table_lookup(dev->params, STONITH_ATTR_HOSTMAP)
                   && g_hash_table_lookup(dev->aliases, host)) {
            can = TRUE;
        }

    } else if (safe_str_eq(check_type, "dynamic-list")) {
        time_t now = time(NULL);

        if (dev->targets == NULL || dev->targets_age + 60 < now) {
            crm_trace("Running %s command to see if %s can fence %s (%s)",
                      check_type, dev?dev->id:"N/A", search->host, search->action);

            schedule_internal_command(__FUNCTION__, dev, "list", NULL,
                                      search->per_device_timeout, search, dynamic_list_search_cb);

            /* we'll respond to this search request async in the cb */
            return;
        }

        if (string_in_list(dev->targets, alias)) {
            can = TRUE;
        }

    } else if (safe_str_eq(check_type, "status")) {
        crm_trace("Running %s command to see if %s can fence %s (%s)",
                  check_type, dev?dev->id:"N/A", search->host, search->action);
        schedule_internal_command(__FUNCTION__, dev, "status", search->host,
                                  search->per_device_timeout, search, status_search_cb);
        /* we'll respond to this search request async in the cb */
        return;
    } else {
        crm_err("Unknown check type: %s", check_type);
    }

    if (safe_str_eq(host, alias)) {
        crm_notice("%s can%s fence (%s) %s: %s", dev->id, can ? "" : " not", search->action, host, check_type);
    } else {
        crm_notice("%s can%s fence (%s) %s (aka. '%s'): %s", dev->id, can ? "" : " not", search->action, host, alias,
                   check_type);
    }

  search_report_results:
    search_devices_record_result(search, dev ? dev->id : NULL, can);
}

static void
search_devices(gpointer key, gpointer value, gpointer user_data)
{
    stonith_device_t *dev = value;
    struct device_search_s *search = user_data;

    can_fence_host_with_device(dev, search);
}

#define DEFAULT_QUERY_TIMEOUT 20
static void
get_capable_devices(const char *host, const char *action, int timeout, bool suicide, void *user_data,
                    void (*callback) (GList * devices, void *user_data))
{
    struct device_search_s *search;
    int per_device_timeout = DEFAULT_QUERY_TIMEOUT;
    int devices_needing_async_query = 0;
    char *key = NULL;
    const char *check_type = NULL;
    GHashTableIter gIter;
    stonith_device_t *device = NULL;

    if (!g_hash_table_size(device_list)) {
        callback(NULL, user_data);
        return;
    }

    search = calloc(1, sizeof(struct device_search_s));
    if (!search) {
        callback(NULL, user_data);
        return;
    }

    g_hash_table_iter_init(&gIter, device_list);
    while (g_hash_table_iter_next(&gIter, (void **)&key, (void **)&device)) {
        check_type = target_list_type(device);
        if (safe_str_eq(check_type, "status") || safe_str_eq(check_type, "dynamic-list")) {
            devices_needing_async_query++;
        }
    }

    /* If we have devices that require an async event in order to know what
     * nodes they can fence, we have to give the events a timeout. The total
     * query timeout is divided among those events. */
    if (devices_needing_async_query) {
        per_device_timeout = timeout / devices_needing_async_query;
        if (!per_device_timeout) {
            crm_err("STONITH timeout %ds is too low; using %ds, but consider raising to at least %ds",
                    timeout, DEFAULT_QUERY_TIMEOUT,
                    DEFAULT_QUERY_TIMEOUT * devices_needing_async_query);
            per_device_timeout = DEFAULT_QUERY_TIMEOUT;
        } else if (per_device_timeout < DEFAULT_QUERY_TIMEOUT) {
            crm_notice("STONITH timeout %ds is low for the current configuration;"
                       " consider raising to at least %ds",
                       timeout, DEFAULT_QUERY_TIMEOUT * devices_needing_async_query);
        }
    }

    search->host = host ? strdup(host) : NULL;
    search->action = action ? strdup(action) : NULL;
    search->per_device_timeout = per_device_timeout;
    /* We are guaranteed this many replies. Even if a device gets
     * unregistered some how during the async search, we will get
     * the correct number of replies. */
    search->replies_needed = g_hash_table_size(device_list);
    search->allow_suicide = suicide;
    search->callback = callback;
    search->user_data = user_data;
    /* kick off the search */

    crm_debug("Searching through %d devices to see what is capable of action (%s) for target %s",
              search->replies_needed,
              search->action ? search->action : "<unknown>",
              search->host ? search->host : "<anyone>");
    g_hash_table_foreach(device_list, search_devices, search);
}

struct st_query_data {
    xmlNode *reply;
    char *remote_peer;
    char *client_id;
    char *target;
    char *action;
    int call_options;
};

/*
 * \internal
 * \brief Add action-specific attributes to query reply XML
 *
 * \param[in,out] xml     XML to add attributes to
 * \param[in]     action  Fence action
 * \param[in]     device  Fence device
 */
static void
add_action_specific_attributes(xmlNode *xml, const char *action,
                               stonith_device_t *device)
{
    int action_specific_timeout;
    int delay_max;

    CRM_CHECK(xml && action && device, return);

    if (is_action_required(action, device)) {
        crm_trace("Action %s is required on %s", action, device->id);
        crm_xml_add_int(xml, F_STONITH_DEVICE_REQUIRED, 1);
    }

    action_specific_timeout = get_action_timeout(device, action, 0);
    if (action_specific_timeout) {
        crm_trace("Action %s has timeout %dms on %s",
                  action, action_specific_timeout, device->id);
        crm_xml_add_int(xml, F_STONITH_ACTION_TIMEOUT, action_specific_timeout);
    }

    delay_max = get_action_delay_max(device, action);
    if (delay_max > 0) {
        crm_trace("Action %s has maximum random delay %dms on %s",
                  action, delay_max, device->id);
        crm_xml_add_int(xml, F_STONITH_DELAY_MAX, delay_max / 1000);
    }
}

/*
 * \internal
 * \brief Add "disallowed" attribute to query reply XML if appropriate
 *
 * \param[in,out] xml            XML to add attribute to
 * \param[in]     action         Fence action
 * \param[in]     device         Fence device
 * \param[in]     target         Fence target
 * \param[in]     allow_suicide  Whether self-fencing is allowed
 */
static void
add_disallowed(xmlNode *xml, const char *action, stonith_device_t *device,
               const char *target, gboolean allow_suicide)
{
    if (!localhost_is_eligible(device, action, target, allow_suicide)) {
        crm_trace("Action %s on %s is disallowed for local host",
                  action, device->id);
        crm_xml_add(xml, F_STONITH_ACTION_DISALLOWED, XML_BOOLEAN_TRUE);
    }
}

/*
 * \internal
 * \brief Add child element with action-specific values to query reply XML
 *
 * \param[in,out] xml            XML to add attribute to
 * \param[in]     action         Fence action
 * \param[in]     device         Fence device
 * \param[in]     target         Fence target
 * \param[in]     allow_suicide  Whether self-fencing is allowed
 */
static void
add_action_reply(xmlNode *xml, const char *action, stonith_device_t *device,
               const char *target, gboolean allow_suicide)
{
    xmlNode *child = create_xml_node(xml, F_STONITH_ACTION);

    crm_xml_add(child, XML_ATTR_ID, action);
    add_action_specific_attributes(child, action, device);
    add_disallowed(child, action, device, target, allow_suicide);
}

static void
stonith_query_capable_device_cb(GList * devices, void *user_data)
{
    struct st_query_data *query = user_data;
    int available_devices = 0;
    xmlNode *dev = NULL;
    xmlNode *list = NULL;
    GListPtr lpc = NULL;

    /* Pack the results into XML */
    list = create_xml_node(NULL, __FUNCTION__);
    crm_xml_add(list, F_STONITH_TARGET, query->target);
    for (lpc = devices; lpc != NULL; lpc = lpc->next) {
        stonith_device_t *device = g_hash_table_lookup(device_list, lpc->data);
        const char *action = query->action;

        if (!device) {
            /* It is possible the device got unregistered while
             * determining who can fence the target */
            continue;
        }

        available_devices++;

        dev = create_xml_node(list, F_STONITH_DEVICE);
        crm_xml_add(dev, XML_ATTR_ID, device->id);
        crm_xml_add(dev, "namespace", device->namespace);
        crm_xml_add(dev, "agent", device->agent);
        crm_xml_add_int(dev, F_STONITH_DEVICE_VERIFIED, device->verified);

        /* If the originating stonithd wants to reboot the node, and we have a
         * capable device that doesn't support "reboot", remap to "off" instead.
         */
        if (is_not_set(device->flags, st_device_supports_reboot)
            && safe_str_eq(query->action, "reboot")) {
            crm_trace("%s doesn't support reboot, using values for off instead",
                      device->id);
            action = "off";
        }

        /* Add action-specific values if available */
        add_action_specific_attributes(dev, action, device);
        if (safe_str_eq(query->action, "reboot")) {
            /* A "reboot" *might* get remapped to "off" then "on", so after
             * sending the "reboot"-specific values in the main element, we add
             * sub-elements for "off" and "on" values.
             *
             * We short-circuited earlier if "reboot", "off" and "on" are all
             * disallowed for the local host. However if only one or two are
             * disallowed, we send back the results and mark which ones are
             * disallowed. If "reboot" is disallowed, this might cause problems
             * with older stonithd versions, which won't check for it. Older
             * versions will ignore "off" and "on", so they are not a problem.
             */
            add_disallowed(dev, action, device, query->target,
                           is_set(query->call_options, st_opt_allow_suicide));
            add_action_reply(dev, "off", device, query->target,
                             is_set(query->call_options, st_opt_allow_suicide));
            add_action_reply(dev, "on", device, query->target, FALSE);
        }

        /* A query without a target wants device parameters */
        if (query->target == NULL) {
            xmlNode *attrs = create_xml_node(dev, XML_TAG_ATTRS);

            g_hash_table_foreach(device->params, hash2field, attrs);
        }
    }

    crm_xml_add_int(list, F_STONITH_AVAILABLE_DEVICES, available_devices);
    if (query->target) {
        crm_debug("Found %d matching devices for '%s'", available_devices, query->target);
    } else {
        crm_debug("%d devices installed", available_devices);
    }

    if (list != NULL) {
        crm_log_xml_trace(list, "Add query results");
        add_message_xml(query->reply, F_STONITH_CALLDATA, list);
    }
    stonith_send_reply(query->reply, query->call_options, query->remote_peer, query->client_id);

    free_xml(query->reply);
    free(query->remote_peer);
    free(query->client_id);
    free(query->target);
    free(query->action);
    free(query);
    free_xml(list);
    g_list_free_full(devices, free);
}

static void
stonith_query(xmlNode * msg, const char *remote_peer, const char *client_id, int call_options)
{
    struct st_query_data *query = NULL;
    const char *action = NULL;
    const char *target = NULL;
    int timeout = 0;
    xmlNode *dev = get_xpath_object("//@" F_STONITH_ACTION, msg, LOG_DEBUG_3);

    crm_element_value_int(msg, F_STONITH_TIMEOUT, &timeout);
    if (dev) {
        const char *device = crm_element_value(dev, F_STONITH_DEVICE);

        target = crm_element_value(dev, F_STONITH_TARGET);
        action = crm_element_value(dev, F_STONITH_ACTION);
        if (device && safe_str_eq(device, "manual_ack")) {
            /* No query or reply necessary */
            return;
        }
    }

    crm_log_xml_debug(msg, "Query");
    query = calloc(1, sizeof(struct st_query_data));

    query->reply = stonith_construct_reply(msg, NULL, NULL, pcmk_ok);
    query->remote_peer = remote_peer ? strdup(remote_peer) : NULL;
    query->client_id = client_id ? strdup(client_id) : NULL;
    query->target = target ? strdup(target) : NULL;
    query->action = action ? strdup(action) : NULL;
    query->call_options = call_options;

    get_capable_devices(target, action, timeout,
                        is_set(call_options, st_opt_allow_suicide),
                        query, stonith_query_capable_device_cb);
}

#define ST_LOG_OUTPUT_MAX 512
static void
log_operation(async_command_t * cmd, int rc, int pid, const char *next, const char *output)
{
    if (rc == 0) {
        next = NULL;
    }

    if (cmd->victim != NULL) {
        do_crm_log(rc == 0 ? LOG_NOTICE : LOG_ERR,
                   "Operation '%s' [%d] (call %d from %s) for host '%s' with device '%s' returned: %d (%s)%s%s",
                   cmd->action, pid, cmd->id, cmd->client_name, cmd->victim, cmd->device, rc,
                   pcmk_strerror(rc), next ? ". Trying: " : "", next ? next : "");
    } else {
        do_crm_log_unlikely(rc == 0 ? LOG_DEBUG : LOG_NOTICE,
                            "Operation '%s' [%d] for device '%s' returned: %d (%s)%s%s",
                            cmd->action, pid, cmd->device, rc, pcmk_strerror(rc),
                            next ? ". Trying: " : "", next ? next : "");
    }

    if (output) {
        /* Logging the whole string confuses syslog when the string is xml */
        char *prefix = crm_strdup_printf("%s:%d", cmd->device, pid);

        crm_log_output(rc == 0 ? LOG_DEBUG : LOG_WARNING, prefix, output);
        free(prefix);
    }
}

static void
stonith_send_async_reply(async_command_t * cmd, const char *output, int rc, GPid pid)
{
    xmlNode *reply = NULL;
    gboolean bcast = FALSE;

    reply = stonith_construct_async_reply(cmd, output, NULL, rc);

    if (safe_str_eq(cmd->action, "metadata")) {
        /* Too verbose to log */
        crm_trace("Metadata query for %s", cmd->device);
        output = NULL;

    } else if (crm_str_eq(cmd->action, "monitor", TRUE) ||
               crm_str_eq(cmd->action, "list", TRUE) || crm_str_eq(cmd->action, "status", TRUE)) {
        crm_trace("Never broadcast %s replies", cmd->action);

    } else if (!stand_alone && safe_str_eq(cmd->origin, cmd->victim) && safe_str_neq(cmd->action, "on")) {
        crm_trace("Broadcast %s reply for %s", cmd->action, cmd->victim);
        crm_xml_add(reply, F_SUBTYPE, "broadcast");
        bcast = TRUE;
    }

    log_operation(cmd, rc, pid, NULL, output);
    crm_log_xml_trace(reply, "Reply");

    if (bcast) {
        crm_xml_add(reply, F_STONITH_OPERATION, T_STONITH_NOTIFY);
        send_cluster_message(NULL, crm_msg_stonith_ng, reply, FALSE);

    } else if (cmd->origin) {
        crm_trace("Directed reply to %s", cmd->origin);
        send_cluster_message(crm_get_peer(0, cmd->origin), crm_msg_stonith_ng, reply, FALSE);

    } else {
        crm_trace("Directed local %ssync reply to %s",
                  (cmd->options & st_opt_sync_call) ? "" : "a-", cmd->client_name);
        do_local_reply(reply, cmd->client, cmd->options & st_opt_sync_call, FALSE);
    }

    if (stand_alone) {
        /* Do notification with a clean data object */
        xmlNode *notify_data = create_xml_node(NULL, T_STONITH_NOTIFY_FENCE);

        crm_xml_add_int(notify_data, F_STONITH_RC, rc);
        crm_xml_add(notify_data, F_STONITH_TARGET, cmd->victim);
        crm_xml_add(notify_data, F_STONITH_OPERATION, cmd->op);
        crm_xml_add(notify_data, F_STONITH_DELEGATE, "localhost");
        crm_xml_add(notify_data, F_STONITH_DEVICE, cmd->device);
        crm_xml_add(notify_data, F_STONITH_REMOTE_OP_ID, cmd->remote_op_id);
        crm_xml_add(notify_data, F_STONITH_ORIGIN, cmd->client);

        do_stonith_notify(0, T_STONITH_NOTIFY_FENCE, rc, notify_data);
    }

    free_xml(reply);
}

void
unfence_cb(GPid pid, int rc, const char *output, gpointer user_data)
{
    async_command_t * cmd = user_data;
    stonith_device_t *dev = g_hash_table_lookup(device_list, cmd->device);

    log_operation(cmd, rc, pid, NULL, output);

    if(dev) {
        dev->active_pid = 0;
        mainloop_set_trigger(dev->work);
    } else {
        crm_trace("Device %s does not exist", cmd->device);
    }

    if(rc != 0) {
        crm_exit(DAEMON_RESPAWN_STOP);
    }
}

static void
cancel_stonith_command(async_command_t * cmd)
{
    stonith_device_t *device;

    CRM_CHECK(cmd != NULL, return);

    if (!cmd->device) {
        return;
    }

    device = g_hash_table_lookup(device_list, cmd->device);

    if (device) {
        crm_trace("Cancel scheduled %s on %s", cmd->action, device->id);
        device->pending_ops = g_list_remove(device->pending_ops, cmd);
    }
}

static void
st_child_done(GPid pid, int rc, const char *output, gpointer user_data)
{
    stonith_device_t *device = NULL;
    stonith_device_t *next_device = NULL;
    async_command_t *cmd = user_data;

    GListPtr gIter = NULL;
    GListPtr gIterNext = NULL;

    CRM_CHECK(cmd != NULL, return);

    /* The device is ready to do something else now */
    device = g_hash_table_lookup(device_list, cmd->device);
    if (device) {
        device->active_pid = 0;
        if (rc == pcmk_ok &&
            (safe_str_eq(cmd->action, "list") ||
             safe_str_eq(cmd->action, "monitor") || safe_str_eq(cmd->action, "status"))) {

            device->verified = TRUE;
        }

        mainloop_set_trigger(device->work);
    }

    crm_debug("Operation '%s' on '%s' completed with rc=%d (%d remaining)",
              cmd->action, cmd->device, rc, g_list_length(cmd->device_next));

    if (rc == 0) {
        GListPtr iter;
        /* see if there are any required devices left to execute for this op */
        for (iter = cmd->device_next; iter != NULL; iter = iter->next) {
            next_device = g_hash_table_lookup(device_list, iter->data);

            if (next_device != NULL && is_action_required(cmd->action, next_device)) {
                cmd->device_next = iter->next;
                break;
            }
            next_device = NULL;
        }

    } else if (rc != 0 && cmd->device_next && (is_action_required(cmd->action, device) == FALSE)) {
        /* if this device didn't work out, see if there are any others we can try.
         * if the failed device was 'required', we can't pick another device. */
        next_device = g_hash_table_lookup(device_list, cmd->device_next->data);
        cmd->device_next = cmd->device_next->next;
    }

    /* this operation requires more fencing, hooray! */
    if (next_device) {
        log_operation(cmd, rc, pid, cmd->device, output);

        schedule_stonith_command(cmd, next_device);
        /* Prevent cmd from being freed */
        cmd = NULL;
        goto done;
    }

    stonith_send_async_reply(cmd, output, rc, pid);

    if (rc != 0) {
        goto done;
    }

    /* Check to see if any operations are scheduled to do the exact
     * same thing that just completed.  If so, rather than
     * performing the same fencing operation twice, return the result
     * of this operation for all pending commands it matches. */
    for (gIter = cmd_list; gIter != NULL; gIter = gIterNext) {
        async_command_t *cmd_other = gIter->data;

        gIterNext = gIter->next;

        if (cmd == cmd_other) {
            continue;
        }

        /* A pending scheduled command matches the command that just finished if.
         * 1. The client connections are different.
         * 2. The node victim is the same.
         * 3. The fencing action is the same.
         * 4. The device scheduled to execute the action is the same.
         */
        if (safe_str_eq(cmd->client, cmd_other->client) ||
            safe_str_neq(cmd->victim, cmd_other->victim) ||
            safe_str_neq(cmd->action, cmd_other->action) ||
            safe_str_neq(cmd->device, cmd_other->device)) {

            continue;
        }

        /* Duplicate merging will do the right thing for either type of remapped
         * reboot. If the executing stonithd remapped an unsupported reboot to
         * off, then cmd->action will be reboot and will be merged with any
         * other reboot requests. If the originating stonithd remapped a
         * topology reboot to off then on, we will get here once with
         * cmd->action "off" and once with "on", and they will be merged
         * separately with similar requests.
         */
        crm_notice
            ("Merging stonith action %s for node %s originating from client %s with identical stonith request from client %s",
             cmd_other->action, cmd_other->victim, cmd_other->client_name, cmd->client_name);

        cmd_list = g_list_remove_link(cmd_list, gIter);

        stonith_send_async_reply(cmd_other, output, rc, pid);
        cancel_stonith_command(cmd_other);

        free_async_command(cmd_other);
        g_list_free_1(gIter);
    }

  done:
    free_async_command(cmd);
}

static gint
sort_device_priority(gconstpointer a, gconstpointer b)
{
    const stonith_device_t *dev_a = a;
    const stonith_device_t *dev_b = b;

    if (dev_a->priority > dev_b->priority) {
        return -1;
    } else if (dev_a->priority < dev_b->priority) {
        return 1;
    }
    return 0;
}

static void
stonith_fence_get_devices_cb(GList * devices, void *user_data)
{
    async_command_t *cmd = user_data;
    stonith_device_t *device = NULL;

    crm_info("Found %d matching devices for '%s'", g_list_length(devices), cmd->victim);

    if (g_list_length(devices) > 0) {
        /* Order based on priority */
        devices = g_list_sort(devices, sort_device_priority);
        device = g_hash_table_lookup(device_list, devices->data);

        if (device) {
            cmd->device_list = devices;
            cmd->device_next = devices->next;
            devices = NULL;     /* list owned by cmd now */
        }
    }

    /* we have a device, schedule it for fencing. */
    if (device) {
        schedule_stonith_command(cmd, device);
        /* in progress */
        return;
    }

    /* no device found! */
    stonith_send_async_reply(cmd, NULL, -ENODEV, 0);

    free_async_command(cmd);
    g_list_free_full(devices, free);
}

static int
stonith_fence(xmlNode * msg)
{
    const char *device_id = NULL;
    stonith_device_t *device = NULL;
    async_command_t *cmd = create_async_command(msg);
    xmlNode *dev = get_xpath_object("//@" F_STONITH_TARGET, msg, LOG_ERR);

    if (cmd == NULL) {
        return -EPROTO;
    }

    device_id = crm_element_value(dev, F_STONITH_DEVICE);
    if (device_id) {
        device = g_hash_table_lookup(device_list, device_id);
        if (device == NULL) {
            crm_err("Requested device '%s' is not available", device_id);
            return -ENODEV;
        }
        schedule_stonith_command(cmd, device);

    } else {
        const char *host = crm_element_value(dev, F_STONITH_TARGET);

        if (cmd->options & st_opt_cs_nodeid) {
            int nodeid = crm_atoi(host, NULL);
            crm_node_t *node = crm_get_peer(nodeid, NULL);

            if (node) {
                host = node->uname;
            }
        }

        /* If we get to here, then self-fencing is implicitly allowed */
        get_capable_devices(host, cmd->action, cmd->default_timeout,
                            TRUE, cmd, stonith_fence_get_devices_cb);
    }

    return -EINPROGRESS;
}

xmlNode *
stonith_construct_reply(xmlNode * request, const char *output, xmlNode * data, int rc)
{
    int lpc = 0;
    xmlNode *reply = NULL;

    const char *name = NULL;
    const char *value = NULL;

    const char *names[] = {
        F_STONITH_OPERATION,
        F_STONITH_CALLID,
        F_STONITH_CLIENTID,
        F_STONITH_CLIENTNAME,
        F_STONITH_REMOTE_OP_ID,
        F_STONITH_CALLOPTS
    };

    crm_trace("Creating a basic reply");
    reply = create_xml_node(NULL, T_STONITH_REPLY);

    crm_xml_add(reply, "st_origin", __FUNCTION__);
    crm_xml_add(reply, F_TYPE, T_STONITH_NG);
    crm_xml_add(reply, "st_output", output);
    crm_xml_add_int(reply, F_STONITH_RC, rc);

    CRM_CHECK(request != NULL, crm_warn("Can't create a sane reply"); return reply);
    for (lpc = 0; lpc < DIMOF(names); lpc++) {
        name = names[lpc];
        value = crm_element_value(request, name);
        crm_xml_add(reply, name, value);
    }

    if (data != NULL) {
        crm_trace("Attaching reply output");
        add_message_xml(reply, F_STONITH_CALLDATA, data);
    }
    return reply;
}

static xmlNode *
stonith_construct_async_reply(async_command_t * cmd, const char *output, xmlNode * data, int rc)
{
    xmlNode *reply = NULL;

    crm_trace("Creating a basic reply");
    reply = create_xml_node(NULL, T_STONITH_REPLY);

    crm_xml_add(reply, "st_origin", __FUNCTION__);
    crm_xml_add(reply, F_TYPE, T_STONITH_NG);

    crm_xml_add(reply, F_STONITH_OPERATION, cmd->op);
    crm_xml_add(reply, F_STONITH_DEVICE, cmd->device);
    crm_xml_add(reply, F_STONITH_REMOTE_OP_ID, cmd->remote_op_id);
    crm_xml_add(reply, F_STONITH_CLIENTID, cmd->client);
    crm_xml_add(reply, F_STONITH_CLIENTNAME, cmd->client_name);
    crm_xml_add(reply, F_STONITH_TARGET, cmd->victim);
    crm_xml_add(reply, F_STONITH_ACTION, cmd->op);
    crm_xml_add(reply, F_STONITH_ORIGIN, cmd->origin);
    crm_xml_add_int(reply, F_STONITH_CALLID, cmd->id);
    crm_xml_add_int(reply, F_STONITH_CALLOPTS, cmd->options);

    crm_xml_add_int(reply, F_STONITH_RC, rc);

    crm_xml_add(reply, "st_output", output);

    if (data != NULL) {
        crm_info("Attaching reply output");
        add_message_xml(reply, F_STONITH_CALLDATA, data);
    }
    return reply;
}

bool fencing_peer_active(crm_node_t *peer)
{
    if (peer == NULL) {
        return FALSE;
    } else if (peer->uname == NULL) {
        return FALSE;
    } else if (is_set(peer->processes, crm_get_cluster_proc())) {
        return TRUE;
    }
    return FALSE;
}

/*!
 * \internal
 * \brief Determine if we need to use an alternate node to
 * fence the target. If so return that node's uname
 *
 * \retval NULL, no alternate host
 * \retval uname, uname of alternate host to use
 */
static const char *
check_alternate_host(const char *target)
{
    const char *alternate_host = NULL;

    crm_trace("Checking if we (%s) can fence %s", stonith_our_uname, target);
    if (find_topology_for_host(target) && safe_str_eq(target, stonith_our_uname)) {
        GHashTableIter gIter;
        crm_node_t *entry = NULL;

        g_hash_table_iter_init(&gIter, crm_peer_cache);
        while (g_hash_table_iter_next(&gIter, NULL, (void **)&entry)) {
            crm_trace("Checking for %s.%d != %s", entry->uname, entry->id, target);
            if (fencing_peer_active(entry)
                && safe_str_neq(entry->uname, target)) {
                alternate_host = entry->uname;
                break;
            }
        }
        if (alternate_host == NULL) {
            crm_err("No alternate host available to handle complex self fencing request");
            g_hash_table_iter_init(&gIter, crm_peer_cache);
            while (g_hash_table_iter_next(&gIter, NULL, (void **)&entry)) {
                crm_notice("Peer[%d] %s", entry->id, entry->uname);
            }
        }
    }

    return alternate_host;
}

static void
stonith_send_reply(xmlNode * reply, int call_options, const char *remote_peer,
                   const char *client_id)
{
    if (remote_peer) {
        send_cluster_message(crm_get_peer(0, remote_peer), crm_msg_stonith_ng, reply, FALSE);
    } else {
        do_local_reply(reply, client_id, is_set(call_options, st_opt_sync_call), remote_peer != NULL);
    }
}

static int
handle_request(crm_client_t * client, uint32_t id, uint32_t flags, xmlNode * request,
               const char *remote_peer)
{
    int call_options = 0;
    int rc = -EOPNOTSUPP;

    xmlNode *data = NULL;
    xmlNode *reply = NULL;

    char *output = NULL;
    const char *op = crm_element_value(request, F_STONITH_OPERATION);
    const char *client_id = crm_element_value(request, F_STONITH_CLIENTID);

    crm_element_value_int(request, F_STONITH_CALLOPTS, &call_options);

    if (is_set(call_options, st_opt_sync_call)) {
        CRM_ASSERT(client == NULL || client->request_id == id);
    }

    if (crm_str_eq(op, CRM_OP_REGISTER, TRUE)) {
        xmlNode *reply = create_xml_node(NULL, "reply");

        CRM_ASSERT(client);
        crm_xml_add(reply, F_STONITH_OPERATION, CRM_OP_REGISTER);
        crm_xml_add(reply, F_STONITH_CLIENTID, client->id);
        crm_ipcs_send(client, id, reply, flags);
        client->request_id = 0;
        free_xml(reply);
        return 0;

    } else if (crm_str_eq(op, STONITH_OP_EXEC, TRUE)) {
        rc = stonith_device_action(request, &output);

    } else if (crm_str_eq(op, STONITH_OP_TIMEOUT_UPDATE, TRUE)) {
        const char *call_id = crm_element_value(request, F_STONITH_CALLID);
        const char *client_id = crm_element_value(request, F_STONITH_CLIENTID);
        int op_timeout = 0;

        crm_element_value_int(request, F_STONITH_TIMEOUT, &op_timeout);
        do_stonith_async_timeout_update(client_id, call_id, op_timeout);
        return 0;

    } else if (crm_str_eq(op, STONITH_OP_QUERY, TRUE)) {
        if (remote_peer) {
            create_remote_stonith_op(client_id, request, TRUE); /* Record it for the future notification */
        }
        stonith_query(request, remote_peer, client_id, call_options);
        return 0;

    } else if (crm_str_eq(op, T_STONITH_NOTIFY, TRUE)) {
        const char *flag_name = NULL;

        CRM_ASSERT(client);
        flag_name = crm_element_value(request, F_STONITH_NOTIFY_ACTIVATE);
        if (flag_name) {
            crm_debug("Setting %s callbacks for %s (%s): ON", flag_name, client->name, client->id);
            client->options |= get_stonith_flag(flag_name);
        }

        flag_name = crm_element_value(request, F_STONITH_NOTIFY_DEACTIVATE);
        if (flag_name) {
            crm_debug("Setting %s callbacks for %s (%s): off", flag_name, client->name, client->id);
            client->options |= get_stonith_flag(flag_name);
        }

        if (flags & crm_ipc_client_response) {
            crm_ipcs_send_ack(client, id, flags, "ack", __FUNCTION__, __LINE__);
        }
        return 0;

    } else if (crm_str_eq(op, STONITH_OP_RELAY, TRUE)) {
        xmlNode *dev = get_xpath_object("//@" F_STONITH_TARGET, request, LOG_TRACE);

        crm_notice("Peer %s has received a forwarded fencing request from %s to fence (%s) peer %s",
                   stonith_our_uname,
                   client ? client->name : remote_peer,
                   crm_element_value(dev, F_STONITH_ACTION),
                   crm_element_value(dev, F_STONITH_TARGET));

        if (initiate_remote_stonith_op(NULL, request, FALSE) != NULL) {
            rc = -EINPROGRESS;
        }

    } else if (crm_str_eq(op, STONITH_OP_FENCE, TRUE)) {

        if (remote_peer || stand_alone) {
            rc = stonith_fence(request);

        } else if (call_options & st_opt_manual_ack) {
            remote_fencing_op_t *rop = NULL;
            xmlNode *dev = get_xpath_object("//@" F_STONITH_TARGET, request, LOG_TRACE);
            const char *target = crm_element_value(dev, F_STONITH_TARGET);

            crm_notice("Received manual confirmation that %s is fenced", target);
            rop = initiate_remote_stonith_op(client, request, TRUE);
            rc = stonith_manual_ack(request, rop);

        } else {
            const char *alternate_host = NULL;
            xmlNode *dev = get_xpath_object("//@" F_STONITH_TARGET, request, LOG_TRACE);
            const char *target = crm_element_value(dev, F_STONITH_TARGET);
            const char *action = crm_element_value(dev, F_STONITH_ACTION);
            const char *device = crm_element_value(dev, F_STONITH_DEVICE);

            if (client) {
                int tolerance = 0;

                crm_notice("Client %s.%.8s wants to fence (%s) '%s' with device '%s'",
                           client->name, client->id, action, target, device ? device : "(any)");

                crm_element_value_int(dev, F_STONITH_TOLERANCE, &tolerance);

                if (stonith_check_fence_tolerance(tolerance, target, action)) {
                    rc = 0;
                    goto done;
                }

            } else {
                crm_notice("Peer %s wants to fence (%s) '%s' with device '%s'",
                           remote_peer, action, target, device ? device : "(any)");
            }

            alternate_host = check_alternate_host(target);

            if (alternate_host && client) {
                const char *client_id = NULL;

                crm_notice("Forwarding complex self fencing request to peer %s", alternate_host);

                if (client->id) {
                    client_id = client->id;
                } else {
                    client_id = crm_element_value(request, F_STONITH_CLIENTID);
                }

                /* Create a record of it, otherwise call_id will be 0 if we need to notify of failures */
                create_remote_stonith_op(client_id, request, FALSE);

                crm_xml_add(request, F_STONITH_OPERATION, STONITH_OP_RELAY);
                crm_xml_add(request, F_STONITH_CLIENTID, client->id);
                send_cluster_message(crm_get_peer(0, alternate_host), crm_msg_stonith_ng, request,
                                     FALSE);
                rc = -EINPROGRESS;

            } else if (initiate_remote_stonith_op(client, request, FALSE) != NULL) {
                rc = -EINPROGRESS;
            }
        }

    } else if (crm_str_eq(op, STONITH_OP_FENCE_HISTORY, TRUE)) {
        rc = stonith_fence_history(request, &data);

    } else if (crm_str_eq(op, STONITH_OP_DEVICE_ADD, TRUE)) {
        const char *id = NULL;

        rc = stonith_device_register(request, &id, FALSE);
        do_stonith_notify_device(call_options, op, rc, id);

    } else if (crm_str_eq(op, STONITH_OP_DEVICE_DEL, TRUE)) {
        xmlNode *dev = get_xpath_object("//" F_STONITH_DEVICE, request, LOG_ERR);
        const char *id = crm_element_value(dev, XML_ATTR_ID);

        rc = stonith_device_remove(id, FALSE);
        do_stonith_notify_device(call_options, op, rc, id);

    } else if (crm_str_eq(op, STONITH_OP_LEVEL_ADD, TRUE)) {
        char *id = NULL;

        rc = stonith_level_register(request, &id);
        do_stonith_notify_level(call_options, op, rc, id);
        free(id);

    } else if (crm_str_eq(op, STONITH_OP_LEVEL_DEL, TRUE)) {
        char *id = NULL;

        rc = stonith_level_remove(request, &id);
        do_stonith_notify_level(call_options, op, rc, id);

    } else if (crm_str_eq(op, STONITH_OP_CONFIRM, TRUE)) {
        async_command_t *cmd = create_async_command(request);
        xmlNode *reply = stonith_construct_async_reply(cmd, NULL, NULL, 0);

        crm_xml_add(reply, F_STONITH_OPERATION, T_STONITH_NOTIFY);
        crm_notice("Broadcasting manual fencing confirmation for node %s", cmd->victim);
        send_cluster_message(NULL, crm_msg_stonith_ng, reply, FALSE);

        free_async_command(cmd);
        free_xml(reply);

    } else if(safe_str_eq(op, CRM_OP_RM_NODE_CACHE)) {
        int id = 0;
        const char *name = NULL;

        crm_element_value_int(request, XML_ATTR_ID, &id);
        name = crm_element_value(request, XML_ATTR_UNAME);
        reap_crm_member(id, name);

        return pcmk_ok;

    } else {
        crm_err("Unknown %s from %s", op, client ? client->name : remote_peer);
        crm_log_xml_warn(request, "UnknownOp");
    }

  done:

    /* Always reply unless the request is in process still.
     * If in progress, a reply will happen async after the request
     * processing is finished */
    if (rc != -EINPROGRESS) {
        crm_trace("Reply handling: %p %u %u %d %d %s", client, client?client->request_id:0,
                  id, is_set(call_options, st_opt_sync_call), call_options,
                  crm_element_value(request, F_STONITH_CALLOPTS));

        if (is_set(call_options, st_opt_sync_call)) {
            CRM_ASSERT(client == NULL || client->request_id == id);
        }
        reply = stonith_construct_reply(request, output, data, rc);
        stonith_send_reply(reply, call_options, remote_peer, client_id);
    }

    free(output);
    free_xml(data);
    free_xml(reply);

    return rc;
}

static void
handle_reply(crm_client_t * client, xmlNode * request, const char *remote_peer)
{
    const char *op = crm_element_value(request, F_STONITH_OPERATION);

    if (crm_str_eq(op, STONITH_OP_QUERY, TRUE)) {
        process_remote_stonith_query(request);
    } else if (crm_str_eq(op, T_STONITH_NOTIFY, TRUE)) {
        process_remote_stonith_exec(request);
    } else if (crm_str_eq(op, STONITH_OP_FENCE, TRUE)) {
        /* Reply to a complex fencing op */
        process_remote_stonith_exec(request);
    } else {
        crm_err("Unknown %s reply from %s", op, client ? client->name : remote_peer);
        crm_log_xml_warn(request, "UnknownOp");
    }
}

void
stonith_command(crm_client_t * client, uint32_t id, uint32_t flags, xmlNode * request,
                const char *remote_peer)
{
    int call_options = 0;
    int rc = 0;
    gboolean is_reply = FALSE;

    /* Copy op for reporting. The original might get freed by handle_reply()
     * before we use it in crm_debug():
     *     handle_reply()
     *     |- process_remote_stonith_exec()
     *     |-- remote_op_done()
     *     |--- handle_local_reply_and_notify()
     *     |---- crm_xml_add(...F_STONITH_OPERATION...)
     *     |--- free_xml(op->request)
     */
    char *op = crm_element_value_copy(request, F_STONITH_OPERATION);

    if (get_xpath_object("//" T_STONITH_REPLY, request, LOG_DEBUG_3)) {
        is_reply = TRUE;
    }

    crm_element_value_int(request, F_STONITH_CALLOPTS, &call_options);
    crm_debug("Processing %s%s %u from %s (%16x)", op, is_reply ? " reply" : "",
              id, client ? client->name : remote_peer, call_options);

    if (is_set(call_options, st_opt_sync_call)) {
        CRM_ASSERT(client == NULL || client->request_id == id);
    }

    if (is_reply) {
        handle_reply(client, request, remote_peer);
    } else {
        rc = handle_request(client, id, flags, request, remote_peer);
    }

    crm_debug("Processed %s%s from %s: %s (%d)", op,
              is_reply ? " reply" : "", client ? client->name : remote_peer,
              rc > 0 ? "" : pcmk_strerror(rc), rc);

    free(op);
}
