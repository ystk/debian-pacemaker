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
#include <unistd.h>

#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>

#include <libgen.h>             /* for basename() */

#include <crm/crm.h>
#include <crm/cluster/internal.h>
#include <crm/common/mainloop.h>
#include <crm/msg_xml.h>
#include <crm/cib.h>
#include <crm/attrd.h>

int command = 0;
int ccm_fd = 0;
gboolean do_quiet = FALSE;

char *target_uuid = NULL;
char *target_uname = NULL;
const char *standby_value = NULL;
const char *standby_scope = NULL;

/* *INDENT-OFF* */
static struct crm_option long_options[] = {
    /* Top-level Options */
    {"help",       0, 0, '?', "\tThis text"},
    {"version",    0, 0, '$', "\tVersion information"  },
    {"verbose",    0, 0, 'V', "\tIncrease debug output"},
    {"quiet",      0, 0, 'Q', "\tEssential output only"},

    {"-spacer-",   1, 0, '-', "\nStack:"},
#if SUPPORT_CMAN
    {"cman",       0, 0, 'c', "\tOnly try connecting to a cman-based cluster"},
#endif
#if SUPPORT_COROSYNC
    {"openais",    0, 0, 'A', "\tOnly try connecting to an OpenAIS-based cluster"},
#endif
#ifdef SUPPORT_CS_QUORUM
    {"corosync",   0, 0, 'C', "\tOnly try connecting to an Corosync-based cluster"},
#endif
#ifdef SUPPORT_HEARTBEAT
    {"heartbeat",  0, 0, 'H', "Only try connecting to a Heartbeat-based cluster"},
#endif
    
    {"-spacer-",      1, 0, '-', "\nCommands:"},
    {"name",	      0, 0, 'n', "\tDisplay the name used by the cluster for this node"},
    {"name-for-id",   1, 0, 'N', "\tDisplay the name used by the cluster for the node with the specified id"},
    {"epoch",	      0, 0, 'e', "\tDisplay the epoch during which this node joined the cluster"},
    {"quorum",        0, 0, 'q', "\tDisplay a 1 if our partition has quorum, 0 if not"},
    {"list",          0, 0, 'l', "\tDisplay all known members (past and present) of this cluster (Not available for heartbeat clusters)"},
    {"partition",     0, 0, 'p', "Display the members of this partition"},
    {"cluster-id",    0, 0, 'i', "Display this node's cluster id"},
    {"remove",        1, 0, 'R', "(Advanced) Remove the (stopped) node with the specified name from Pacemaker's configuration and caches"},
    {"-spacer-",      1, 0, '-', "In the case of Heartbeat, CMAN and Corosync 2.0, requires that the node has already been removed from the underlying cluster"},

    {"-spacer-", 1, 0, '-', "\nAdditional Options:"},
    {"force",	 0, 0, 'f'},

    {0, 0, 0, 0}
};
/* *INDENT-ON* */

static int
cib_remove_node(uint32_t id, const char *name)
{
    int rc;
    cib_t *cib = NULL;
    xmlNode *node = NULL;
    xmlNode *node_state = NULL;

    crm_trace("Removing %s from the CIB", name);

    /* TODO: Use 'id' instead */
    if(name == NULL && id == 0) {
        return -ENOTUNIQ;
    }

    node = create_xml_node(NULL, XML_CIB_TAG_NODE);
    node_state = create_xml_node(NULL, XML_CIB_TAG_STATE);

    crm_xml_add(node, XML_ATTR_UNAME, name);
    crm_xml_add(node_state, XML_ATTR_UNAME, name);
    if(id) {
        char buffer[64];
        if(snprintf(buffer, 63, "%u", id) > 0) {
            crm_xml_add(node, XML_ATTR_ID, buffer);
            crm_xml_add(node_state, XML_ATTR_ID, buffer);
        }
    }

    cib = cib_new();
    cib->cmds->signon(cib, crm_system_name, cib_command);

    rc = cib->cmds->delete(cib, XML_CIB_TAG_NODES, node, cib_sync_call);
    if (rc != pcmk_ok) {
        printf("Could not remove %s/%u from " XML_CIB_TAG_NODES ": %s", name, id, pcmk_strerror(rc));
    }
    rc = cib->cmds->delete(cib, XML_CIB_TAG_STATUS, node_state, cib_sync_call);
    if (rc != pcmk_ok) {
        printf("Could not remove %s/%u from " XML_CIB_TAG_STATUS ": %s", name, id, pcmk_strerror(rc));
    }

    cib->cmds->signoff(cib);
    cib_delete(cib);
    return rc;
}

int tools_remove_node_cache(const char *node, const char *target);

int tools_remove_node_cache(const char *node, const char *target)
{
    int n = 0;
    int rc = -1;
    char *name = NULL;
    char *admin_uuid = NULL;
    crm_ipc_t *conn = crm_ipc_new(target, 0);
    xmlNode *cmd = NULL;
    xmlNode *hello = NULL;
    char *endptr = NULL;

    if (!conn) {
        return -ENOTCONN;
    }

    if (!crm_ipc_connect(conn)) {
        crm_perror(LOG_ERR, "Connection to %s failed", target);
        crm_ipc_destroy(conn);
        return -ENOTCONN;
    }

    if(safe_str_eq(target, CRM_SYSTEM_CRMD)) {
        admin_uuid = calloc(1, 11);
        snprintf(admin_uuid, 10, "%d", getpid());
        admin_uuid[10] = '\0';

        hello = create_hello_message(admin_uuid, "crm_node", "0", "1");
        rc = crm_ipc_send(conn, hello, 0, 0, NULL);

        free_xml(hello);
        if (rc < 0) {
            free(admin_uuid);
            return rc;
        }
    }


    errno = 0;
    n = strtol(node, &endptr, 10);
    if (errno != 0 || endptr == node || *endptr != '\0') {
        /* Argument was not a nodeid */
        n = 0;
        name = strdup(node);
    } else {
        name = get_node_name(n);
    }

    crm_trace("Removing %s aka. %s (%u) from the membership cache", name, node, n);

    if(safe_str_eq(target, T_ATTRD)) {
        cmd = create_xml_node(NULL, __FUNCTION__);

        crm_xml_add(cmd, F_TYPE, T_ATTRD);
        crm_xml_add(cmd, F_ORIG, crm_system_name);

        crm_xml_add(cmd, F_ATTRD_TASK, ATTRD_OP_PEER_REMOVE);
        crm_xml_add(cmd, F_ATTRD_HOST, name);

        if (n) {
            char buffer[64];
            if(snprintf(buffer, 63, "%u", n) > 0) {
                crm_xml_add(cmd, F_ATTRD_HOST_ID, buffer);
            }
        }

    } else {
        cmd = create_request(CRM_OP_RM_NODE_CACHE,
                             NULL, NULL, target, crm_system_name, admin_uuid);
        if (n) {
            char buffer[64];
            if(snprintf(buffer, 63, "%u", n) > 0) {
                crm_xml_add(cmd, XML_ATTR_ID, buffer);
            }
        }
        crm_xml_add(cmd, XML_ATTR_UNAME, name);
    }

    rc = crm_ipc_send(conn, cmd, 0, 0, NULL);
    crm_debug("%s peer cache cleanup for %s (%u): %d", target, name, n, rc);

    if (rc > 0) {
        rc = cib_remove_node(n, name);
    }

    if (conn) {
        crm_ipc_close(conn);
        crm_ipc_destroy(conn);
    }
    free(admin_uuid);
    free_xml(cmd);
    free(name);
    return rc > 0 ? 0 : rc;
}

static gint
compare_node_uname(gconstpointer a, gconstpointer b)
{
    const crm_node_t *a_node = a;
    const crm_node_t *b_node = b;
    return strcmp(a_node->uname?a_node->uname:"", b_node->uname?b_node->uname:"");
}

static int
node_mcp_dispatch(const char *buffer, ssize_t length, gpointer userdata)
{
    xmlNode *msg = string2xml(buffer);

    if (msg) {
        xmlNode *node = NULL;
        GListPtr nodes = NULL;
        GListPtr iter = NULL;
        const char *quorate = crm_element_value(msg, "quorate");

        crm_log_xml_trace(msg, "message");
        if (command == 'q' && quorate != NULL) {
            fprintf(stdout, "%s\n", quorate);
            crm_exit(pcmk_ok);

        } else if(command == 'q') {
            crm_exit(1);
        }

        for (node = __xml_first_child(msg); node != NULL; node = __xml_next(node)) {
            crm_node_t *peer = calloc(1, sizeof(crm_node_t));

            nodes = g_list_insert_sorted(nodes, peer, compare_node_uname);
            peer->uname = (char*)crm_element_value_copy(node, "uname");
            peer->state = (char*)crm_element_value_copy(node, "state");
            crm_element_value_int(node, "id", (int*)&peer->id);
        }

        for(iter = nodes; iter; iter = iter->next) {
            crm_node_t *peer = iter->data;
            if (command == 'l') {
                fprintf(stdout, "%u %s %s\n", peer->id, peer->uname, peer->state?peer->state:"");

            } else if (command == 'p') {
                if(safe_str_eq(peer->state, CRM_NODE_MEMBER)) {
                    fprintf(stdout, "%s ", peer->uname);
                }

            } else if (command == 'i') {
                if(safe_str_eq(peer->state, CRM_NODE_MEMBER)) {
                    fprintf(stdout, "%u ", peer->id);
                }
            }
        }

        g_list_free_full(nodes, free);
        free_xml(msg);

        if (command == 'p') {
            fprintf(stdout, "\n");
        }

        crm_exit(pcmk_ok);
    }

    return 0;
}

static void
node_mcp_destroy(gpointer user_data)
{
    crm_exit(ENOTCONN);
}

static gboolean
try_pacemaker(int command, enum cluster_type_e stack)
{
    struct ipc_client_callbacks node_callbacks = {
        .dispatch = node_mcp_dispatch,
        .destroy = node_mcp_destroy
    };

    if (stack == pcmk_cluster_heartbeat) {
        /* Nothing to do for them */
        return FALSE;
    }

    switch (command) {
        case 'e':
            /* Age only applies to heartbeat clusters */
            fprintf(stdout, "1\n");
            crm_exit(pcmk_ok);

        case 'R':
            {
                int lpc = 0;
                const char *daemons[] = {
                    CRM_SYSTEM_CRMD,
                    "stonith-ng",
                    T_ATTRD,
                    CRM_SYSTEM_MCP,
                };

                for(lpc = 0; lpc < DIMOF(daemons); lpc++) {
                    if (tools_remove_node_cache(target_uname, daemons[lpc])) {
                        crm_err("Failed to connect to %s to remove node '%s'", daemons[lpc], target_uname);
                        crm_exit(pcmk_err_generic);
                    }
                }
                crm_exit(pcmk_ok);
            }
            break;

        case 'i':
        case 'l':
        case 'q':
        case 'p':
            /* Go to pacemakerd */
            {
                GMainLoop *amainloop = g_main_new(FALSE);
                mainloop_io_t *ipc =
                    mainloop_add_ipc_client(CRM_SYSTEM_MCP, G_PRIORITY_DEFAULT, 0, NULL, &node_callbacks);
                if (ipc != NULL) {
                    /* Sending anything will get us a list of nodes */
                    xmlNode *poke = create_xml_node(NULL, "poke");

                    crm_ipc_send(mainloop_get_ipc_client(ipc), poke, 0, 0, NULL);
                    free_xml(poke);
                    g_main_run(amainloop);
                }
            }
            break;
    }
    return FALSE;
}

#if SUPPORT_HEARTBEAT
#  include <ocf/oc_event.h>
#  include <ocf/oc_membership.h>
#  include <clplumbing/cl_uuid.h>

#  define UUID_LEN 16

oc_ev_t *ccm_token = NULL;
static void *ccm_library = NULL;
void oc_ev_special(const oc_ev_t *, oc_ev_class_t, int);

static gboolean
read_local_hb_uuid(void)
{
    cl_uuid_t uuid;
    char *buffer = NULL;
    long start = 0, read_len = 0;

    FILE *input = fopen(UUID_FILE, "r");

    if (input == NULL) {
        crm_info("Could not open UUID file %s\n", UUID_FILE);
        return FALSE;
    }

    /* see how big the file is */
    start = ftell(input);
    fseek(input, 0L, SEEK_END);
    if (UUID_LEN != ftell(input)) {
        fprintf(stderr, "%s must contain exactly %d bytes\n", UUID_FILE, UUID_LEN);
        abort();
    }

    fseek(input, 0L, start);
    if (start != ftell(input)) {
        fprintf(stderr, "fseek not behaving: %ld vs. %ld\n", start, ftell(input));
        crm_exit(pcmk_err_generic);
    }

    buffer = malloc(50);
    read_len = fread(uuid.uuid, 1, UUID_LEN, input);
    fclose(input);

    if (read_len != UUID_LEN) {
        fprintf(stderr, "Expected and read bytes differ: %d vs. %ld\n", UUID_LEN, read_len);
        crm_exit(pcmk_err_generic);

    } else if (buffer != NULL) {
        cl_uuid_unparse(&uuid, buffer);
        fprintf(stdout, "%s\n", buffer);
        return TRUE;

    } else {
        fprintf(stderr, "No buffer to unparse\n");
        crm_exit(ENODATA);
    }

    free(buffer);
    return FALSE;
}

static void
ccm_age_callback(oc_ed_t event, void *cookie, size_t size, const void *data)
{
    int lpc;
    int node_list_size;
    const oc_ev_membership_t *oc = (const oc_ev_membership_t *)data;

    int (*ccm_api_callback_done) (void *cookie) =
        find_library_function(&ccm_library, CCM_LIBRARY, "oc_ev_callback_done", 1);

    node_list_size = oc->m_n_member;
    if (command == 'q') {
        crm_debug("Processing \"%s\" event.",
                  event == OC_EV_MS_NEW_MEMBERSHIP ? "NEW MEMBERSHIP" :
                  event == OC_EV_MS_NOT_PRIMARY ? "NOT PRIMARY" :
                  event == OC_EV_MS_PRIMARY_RESTORED ? "PRIMARY RESTORED" :
                  event == OC_EV_MS_EVICTED ? "EVICTED" : "NO QUORUM MEMBERSHIP");
        if (ccm_have_quorum(event)) {
            fprintf(stdout, "1\n");
        } else {
            fprintf(stdout, "0\n");
        }

    } else if (command == 'e') {
        crm_debug("Searching %d members for our birth", oc->m_n_member);
    }
    for (lpc = 0; lpc < node_list_size; lpc++) {
        if (command == 'p') {
            fprintf(stdout, "%s ", oc->m_array[oc->m_memb_idx + lpc].node_uname);

        } else if (command == 'e') {
            int (*ccm_api_is_my_nodeid) (const oc_ev_t * token, const oc_node_t * node) =
                find_library_function(&ccm_library, CCM_LIBRARY, "oc_ev_is_my_nodeid", 1);
            if ((*ccm_api_is_my_nodeid) (ccm_token, &(oc->m_array[lpc]))) {
                crm_debug("MATCH: nodeid=%d, uname=%s, born=%d",
                          oc->m_array[oc->m_memb_idx + lpc].node_id,
                          oc->m_array[oc->m_memb_idx + lpc].node_uname,
                          oc->m_array[oc->m_memb_idx + lpc].node_born_on);
                fprintf(stdout, "%d\n", oc->m_array[oc->m_memb_idx + lpc].node_born_on);
            }
        }
    }

    (*ccm_api_callback_done) (cookie);

    if (command == 'p') {
        fprintf(stdout, "\n");
    }
    fflush(stdout);
    crm_exit(pcmk_ok);
}

static gboolean
ccm_age_connect(int *ccm_fd)
{
    gboolean did_fail = FALSE;
    int ret = 0;

    int (*ccm_api_register) (oc_ev_t ** token) =
        find_library_function(&ccm_library, CCM_LIBRARY, "oc_ev_register", 1);

    int (*ccm_api_set_callback) (const oc_ev_t * token,
                                 oc_ev_class_t class,
                                 oc_ev_callback_t * fn,
                                 oc_ev_callback_t ** prev_fn) =
        find_library_function(&ccm_library, CCM_LIBRARY, "oc_ev_set_callback", 1);

    void (*ccm_api_special) (const oc_ev_t *, oc_ev_class_t, int) =
        find_library_function(&ccm_library, CCM_LIBRARY, "oc_ev_special", 1);
    int (*ccm_api_activate) (const oc_ev_t * token, int *fd) =
        find_library_function(&ccm_library, CCM_LIBRARY, "oc_ev_activate", 1);

    crm_debug("Registering with CCM");
    ret = (*ccm_api_register) (&ccm_token);
    if (ret != 0) {
        crm_info("CCM registration failed: %d", ret);
        did_fail = TRUE;
    }

    if (did_fail == FALSE) {
        crm_debug("Setting up CCM callbacks");
        ret = (*ccm_api_set_callback) (ccm_token, OC_EV_MEMB_CLASS, ccm_age_callback, NULL);
        if (ret != 0) {
            crm_warn("CCM callback not set: %d", ret);
            did_fail = TRUE;
        }
    }
    if (did_fail == FALSE) {
        (*ccm_api_special) (ccm_token, OC_EV_MEMB_CLASS, 0 /*don't care */ );

        crm_debug("Activating CCM token");
        ret = (*ccm_api_activate) (ccm_token, ccm_fd);
        if (ret != 0) {
            crm_warn("CCM Activation failed: %d", ret);
            did_fail = TRUE;
        }
    }

    return !did_fail;
}

static gboolean
try_heartbeat(int command, enum cluster_type_e stack)
{
    crm_debug("Attempting to process %c command", command);

    if (command == 'i') {
        if (read_local_hb_uuid()) {
            crm_exit(pcmk_ok);
        }

    } else if (command == 'R') {
        if (tools_remove_node_cache(target_uname, CRM_SYSTEM_CRMD)) {
            crm_err("Failed to connect to "CRM_SYSTEM_CRMD" to remove node '%s'", target_uname);
            crm_exit(pcmk_err_generic);
        }
        crm_exit(pcmk_ok);

    } else if (ccm_age_connect(&ccm_fd)) {
        int rc = 0;
        fd_set rset;
        int (*ccm_api_handle_event) (const oc_ev_t * token) =
            find_library_function(&ccm_library, CCM_LIBRARY, "oc_ev_handle_event", 1);

        while (1) {

            sleep(1);
            FD_ZERO(&rset);
            FD_SET(ccm_fd, &rset);

            errno = 0;
            rc = select(ccm_fd + 1, &rset, NULL, NULL, NULL);

            if (rc > 0 && (*ccm_api_handle_event) (ccm_token) != 0) {
                crm_err("oc_ev_handle_event failed");
                return FALSE;

            } else if (rc < 0 && errno != EINTR) {
                crm_perror(LOG_ERR, "select failed: %d", rc);
                return FALSE;
            }
        }
    }
    return FALSE;
}
#endif

#if SUPPORT_CMAN
#  include <libcman.h>
#  define MAX_NODES 256
static bool valid_cman_name(const char *name, uint32_t nodeid) 
{
    bool rc = TRUE;

    /* Yes, %d, because that's what CMAN does */
    char *fakename = crm_strdup_printf("Node%d", nodeid);

    if(crm_str_eq(fakename, name, TRUE)) {
        rc = FALSE;
        crm_notice("Ignoring inferred name from cman: %s", fakename);
    }
    free(fakename);
    return rc;
}

static gboolean
try_cman(int command, enum cluster_type_e stack)
{

    int rc = -1, lpc = 0, node_count = 0;
    cman_node_t node;
    cman_cluster_t cluster;
    cman_handle_t cman_handle = NULL;
    cman_node_t cman_nodes[MAX_NODES];

    memset(&cluster, 0, sizeof(cluster));

    cman_handle = cman_init(NULL);
    if (cman_handle == NULL || cman_is_active(cman_handle) == FALSE) {
        crm_info("Couldn't connect to cman");
        return FALSE;
    }

    switch (command) {
        case 'R':
            try_pacemaker(command, stack);
            break;

        case 'e':
            /* Age makes no sense (yet?) in a cman cluster */
            fprintf(stdout, "1\n");
            break;

        case 'q':
            fprintf(stdout, "%d\n", cman_is_quorate(cman_handle));
            break;

        case 'l':
        case 'p':
            memset(cman_nodes, 0, MAX_NODES * sizeof(cman_node_t));
            rc = cman_get_nodes(cman_handle, MAX_NODES, &node_count, cman_nodes);
            if (rc != 0) {
                fprintf(stderr, "Couldn't query cman node list: %d %d", rc, errno);
                goto cman_bail;
            }

            for (lpc = 0; lpc < node_count; lpc++) {
                if(valid_cman_name(cman_nodes[lpc].cn_name, cman_nodes[lpc].cn_nodeid) == FALSE) {
                    /* The name was invented, but we need to print something, make it the id instead */
                    printf("%u ", cman_nodes[lpc].cn_nodeid);

                } if (command == 'l') {
                    printf("%s ", cman_nodes[lpc].cn_name);

                } else if (cman_nodes[lpc].cn_nodeid != 0 && cman_nodes[lpc].cn_member) {
                    /* Never allow node ID 0 to be considered a member #315711 */
                    printf("%s ", cman_nodes[lpc].cn_name);
                }
            }
            printf("\n");
            break;

        case 'i':
            memset(&node, 0, sizeof(cman_node_t));
            rc = cman_get_node(cman_handle, CMAN_NODEID_US, &node);
            if (rc != 0) {
                fprintf(stderr, "Couldn't query cman node id: %d %d", rc, errno);
                goto cman_bail;
            }
            fprintf(stdout, "%u\n", node.cn_nodeid);
            break;

        default:
            fprintf(stderr, "Unknown option '%c'\n", command);
            crm_help('?', EX_USAGE);
    }
    cman_finish(cman_handle);
    crm_exit(pcmk_ok);

  cman_bail:
    cman_finish(cman_handle);
    return crm_exit(EINVAL);
}
#endif

#if HAVE_CONFDB
static void
ais_membership_destroy(gpointer user_data)
{
    crm_err("AIS connection terminated");
    ais_fd_sync = -1;
    crm_exit(ENOTCONN);
}

static gint
member_sort(gconstpointer a, gconstpointer b)
{
    const crm_node_t *node_a = a;
    const crm_node_t *node_b = b;

    return strcmp(node_a->uname, node_b->uname);
}

static void
crm_add_member(gpointer key, gpointer value, gpointer user_data)
{
    GList **list = user_data;
    crm_node_t *node = value;

    if (node->uname != NULL) {
        *list = g_list_insert_sorted(*list, node, member_sort);
    }
}

static void
ais_membership_dispatch(cpg_handle_t handle,
                          const struct cpg_name *groupName,
                          uint32_t nodeid, uint32_t pid, void *msg, size_t msg_len)
{
    uint32_t kind = 0;
    const char *from = NULL;
    char *data = pcmk_message_common_cs(handle, nodeid, pid, msg, &kind, &from);

    switch (kind) {
        case crm_class_members:
        case crm_class_notify:
        case crm_class_quorum:
            break;
        default:
            free(data);
            return;

            break;
    }

    if (command == 'q') {
        if (crm_have_quorum) {
            fprintf(stdout, "1\n");
        } else {
            fprintf(stdout, "0\n");
        }

    } else if (command == 'l') {
        GList *nodes = NULL;
        GListPtr lpc = NULL;

        g_hash_table_foreach(crm_peer_cache, crm_add_member, &nodes);
        for (lpc = nodes; lpc != NULL; lpc = lpc->next) {
            crm_node_t *node = (crm_node_t *) lpc->data;

            fprintf(stdout, "%u %s %s\n", node->id, node->uname, node->state);
        }
        fprintf(stdout, "\n");

    } else if (command == 'p') {
        GList *nodes = NULL;
        GListPtr lpc = NULL;

        g_hash_table_foreach(crm_peer_cache, crm_add_member, &nodes);
        for (lpc = nodes; lpc != NULL; lpc = lpc->next) {
            crm_node_t *node = (crm_node_t *) lpc->data;

            if (node->uname && safe_str_eq(node->state, CRM_NODE_MEMBER)) {
                fprintf(stdout, "%s ", node->uname);
            }
        }
        fprintf(stdout, "\n");
    }

    free(data);
    crm_exit(pcmk_ok);

    return;
}
#endif

#ifdef SUPPORT_CS_QUORUM
#  include <corosync/quorum.h>
#  include <corosync/cpg.h>

static gboolean
try_corosync(int command, enum cluster_type_e stack)
{
    int rc = 0;
    int quorate = 0;
    uint32_t quorum_type = 0;
    unsigned int nodeid = 0;
    cpg_handle_t c_handle = 0;
    quorum_handle_t q_handle = 0;

    switch (command) {
        case 'q':
            /* Go direct to the Quorum API */
            rc = quorum_initialize(&q_handle, NULL, &quorum_type);
            if (rc != CS_OK) {
                crm_err("Could not connect to the Quorum API: %d\n", rc);
                return FALSE;
            }

            rc = quorum_getquorate(q_handle, &quorate);
            if (rc != CS_OK) {
                crm_err("Could not obtain the current Quorum API state: %d\n", rc);
                return FALSE;
            }

            if (quorate) {
                fprintf(stdout, "1\n");
            } else {
                fprintf(stdout, "0\n");
            }
            quorum_finalize(q_handle);
            crm_exit(pcmk_ok);

        case 'i':
            /* Go direct to the CPG API */
            rc = cpg_initialize(&c_handle, NULL);
            if (rc != CS_OK) {
                crm_err("Could not connect to the Cluster Process Group API: %d\n", rc);
                return FALSE;
            }

            rc = cpg_local_get(c_handle, &nodeid);
            if (rc != CS_OK) {
                crm_err("Could not get local node id from the CPG API");
                return FALSE;
            }

            fprintf(stdout, "%u\n", nodeid);
            cpg_finalize(c_handle);
            crm_exit(pcmk_ok);

        default:
            try_pacemaker(command, stack);
            break;
    }
    return FALSE;
}
#endif

#if HAVE_CONFDB
static gboolean
try_openais(int command, enum cluster_type_e stack)
{
    static crm_cluster_t cluster;

    cluster.destroy = ais_membership_destroy;
    cluster.cpg.cpg_deliver_fn = ais_membership_dispatch;
    cluster.cpg.cpg_confchg_fn = NULL;

    if (init_cs_connection_once(&cluster)) {

        GMainLoop *amainloop = NULL;

        switch (command) {
            case 'R':
                send_cluster_text(crm_class_rmpeer, target_uname, TRUE, NULL, crm_msg_ais);
                cib_remove_node(0, target_uname);
                crm_exit(pcmk_ok);

            case 'e':
                /* Age makes no sense (yet) in an AIS cluster */
                fprintf(stdout, "1\n");
                crm_exit(pcmk_ok);

            case 'q':
                send_cluster_text(crm_class_quorum, NULL, TRUE, NULL, crm_msg_ais);
                break;

            case 'l':
            case 'p':
                crm_info("Requesting the list of configured nodes");
                send_cluster_text(crm_class_members, __FUNCTION__, TRUE, NULL, crm_msg_ais);
                break;

            case 'i':
                printf("%u\n", cluster.nodeid);
                crm_exit(pcmk_ok);

            default:
                fprintf(stderr, "Unknown option '%c'\n", command);
                crm_help('?', EX_USAGE);
        }
        amainloop = g_main_new(FALSE);
        g_main_run(amainloop);
    }
    return FALSE;
}
#endif

int set_cluster_type(enum cluster_type_e type);

int
main(int argc, char **argv)
{
    int flag = 0;
    int argerr = 0;
    uint32_t nodeid = 0;
    gboolean force_flag = FALSE;
    gboolean dangerous_cmd = FALSE;
    enum cluster_type_e try_stack = pcmk_cluster_unknown;

    int option_index = 0;

    crm_peer_init();
    crm_log_cli_init("crm_node");
    crm_set_options(NULL, "command [options]", long_options,
                    "Tool for displaying low-level node information");

    while (flag >= 0) {
        flag = crm_get_option(argc, argv, &option_index);
        switch (flag) {
            case -1:
                break;
            case 'V':
                crm_bump_log_level(argc, argv);
                break;
            case '$':
            case '?':
                crm_help(flag, EX_OK);
                break;
            case 'Q':
                do_quiet = TRUE;
                break;
            case 'H':
                set_cluster_type(pcmk_cluster_heartbeat);
                break;
            case 'A':
                set_cluster_type(pcmk_cluster_classic_ais);
                break;
            case 'C':
                set_cluster_type(pcmk_cluster_corosync);
                break;
            case 'c':
                set_cluster_type(pcmk_cluster_cman);
                break;
            case 'f':
                force_flag = TRUE;
                break;
            case 'R':
                command = flag;
                dangerous_cmd = TRUE;
                target_uname = optarg;
                break;
            case 'N':
                command = flag;
                nodeid = crm_parse_int(optarg, NULL);
                break;
            case 'p':
            case 'e':
            case 'q':
            case 'i':
            case 'l':
            case 'n':
                command = flag;
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
        crm_help('?', EX_USAGE);
    }

    if (command == 'n') {
        fprintf(stdout, "%s\n", get_local_node_name());
        crm_exit(pcmk_ok);

    } else if (command == 'N') {
        fprintf(stdout, "%s\n", get_node_name(nodeid));
        crm_exit(pcmk_ok);
    }

    if (dangerous_cmd && force_flag == FALSE) {
        fprintf(stderr, "The supplied command is considered dangerous."
                "  To prevent accidental destruction of the cluster,"
                " the --force flag is required in order to proceed.\n");
        fflush(stderr);
        crm_exit(EINVAL);
    }

    try_stack = get_cluster_type();
    crm_debug("Attempting to process -%c command for cluster type: %s", command,
              name_for_cluster_type(try_stack));

#if SUPPORT_CMAN
    if (try_stack == pcmk_cluster_cman) {
        try_cman(command, try_stack);
    }
#endif

#ifdef SUPPORT_CS_QUORUM
    if (try_stack == pcmk_cluster_corosync) {
        try_corosync(command, try_stack);
    }
#endif

#if HAVE_CONFDB
    /* Only an option if we're using the plugins */
    if (try_stack == pcmk_cluster_classic_ais) {
        try_openais(command, try_stack);
    }
#endif

#if SUPPORT_HEARTBEAT
    if (try_stack == pcmk_cluster_heartbeat) {
        try_heartbeat(command, try_stack);
    }
#endif

    try_pacemaker(command, try_stack);

    return (1);
}
