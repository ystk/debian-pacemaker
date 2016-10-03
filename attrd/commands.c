/*
 * Copyright (C) 2013 Andrew Beekhof <andrew@beekhof.net>
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

#include <sys/types.h>
#include <regex.h>
#include <glib.h>

#include <crm/msg_xml.h>
#include <crm/cluster.h>
#include <crm/cib.h>
#include <crm/cluster/internal.h>
#include <crm/cluster/election.h>
#include <crm/cib/internal.h>

#include <internal.h>

#define ATTRD_PROTOCOL_VERSION "1"

int last_cib_op_done = 0;
char *peer_writer = NULL;
GHashTable *attributes = NULL;

typedef struct attribute_s {
    char *uuid; /* TODO: Remove if at all possible */
    char *id;
    char *set;

    GHashTable *values;

    int update;
    int timeout_ms;

    /* TODO: refactor these three as a bitmask */
    bool changed; /* whether attribute value has changed since last write */
    bool unknown_peer_uuids; /* whether we know we're missing a peer uuid */
    gboolean is_private; /* whether to keep this attribute out of the CIB */

    mainloop_timer_t *timer;

    char *user;

} attribute_t;

typedef struct attribute_value_s {
        uint32_t nodeid;
        gboolean is_remote;
        char *nodename;
        char *current;
        char *requested;
        char *stored;
} attribute_value_t;


void write_attribute(attribute_t *a);
void write_or_elect_attribute(attribute_t *a);
void attrd_peer_update(crm_node_t *peer, xmlNode *xml, const char *host, bool filter);
void attrd_peer_sync(crm_node_t *peer, xmlNode *xml);
void attrd_peer_remove(uint32_t nodeid, const char *host, gboolean uncache, const char *source);

static gboolean
send_attrd_message(crm_node_t * node, xmlNode * data)
{
    crm_xml_add(data, F_TYPE, T_ATTRD);
    crm_xml_add(data, F_ATTRD_IGNORE_LOCALLY, "atomic-version"); /* Tell older versions to ignore our messages */
    crm_xml_add(data, F_ATTRD_VERSION, ATTRD_PROTOCOL_VERSION);
    crm_xml_add_int(data, F_ATTRD_WRITER, election_state(writer));

    return send_cluster_message(node, crm_msg_attrd, data, TRUE);
}

static gboolean
attribute_timer_cb(gpointer data)
{
    attribute_t *a = data;
    crm_trace("Dampen interval expired for %s in state %d", a->id, election_state(writer));
    write_or_elect_attribute(a);
    return FALSE;
}

static void
free_attribute_value(gpointer data)
{
    attribute_value_t *v = data;

    free(v->nodename);
    free(v->current);
    free(v->requested);
    free(v->stored);
    free(v);
}

void
free_attribute(gpointer data)
{
    attribute_t *a = data;
    if(a) {
        free(a->id);
        free(a->set);
        free(a->uuid);
        free(a->user);

        mainloop_timer_del(a->timer);
        g_hash_table_destroy(a->values);

        free(a);
    }
}

static xmlNode *
build_attribute_xml(
    xmlNode *parent, const char *name, const char *set, const char *uuid, unsigned int timeout_ms, const char *user,
    gboolean is_private, const char *peer, uint32_t peerid, const char *value)
{
    xmlNode *xml = create_xml_node(parent, __FUNCTION__);

    crm_xml_add(xml, F_ATTRD_ATTRIBUTE, name);
    crm_xml_add(xml, F_ATTRD_SET, set);
    crm_xml_add(xml, F_ATTRD_KEY, uuid);
    crm_xml_add(xml, F_ATTRD_USER, user);
    crm_xml_add(xml, F_ATTRD_HOST, peer);
    crm_xml_add_int(xml, F_ATTRD_HOST_ID, peerid);
    crm_xml_add(xml, F_ATTRD_VALUE, value);
    crm_xml_add_int(xml, F_ATTRD_DAMPEN, timeout_ms/1000);
    crm_xml_add_int(xml, F_ATTRD_IS_PRIVATE, is_private);

    return xml;
}

static attribute_t *
create_attribute(xmlNode *xml)
{
    int dampen = 0;
    const char *value = crm_element_value(xml, F_ATTRD_DAMPEN);
    attribute_t *a = calloc(1, sizeof(attribute_t));

    a->id      = crm_element_value_copy(xml, F_ATTRD_ATTRIBUTE);
    a->set     = crm_element_value_copy(xml, F_ATTRD_SET);
    a->uuid    = crm_element_value_copy(xml, F_ATTRD_KEY);
    a->values = g_hash_table_new_full(crm_strcase_hash, crm_strcase_equal, NULL, free_attribute_value);

    crm_element_value_int(xml, F_ATTRD_IS_PRIVATE, &a->is_private);

#if ENABLE_ACL
    crm_trace("Performing all %s operations as user '%s'", a->id, a->user);
    a->user = crm_element_value_copy(xml, F_ATTRD_USER);
#endif

    if(value) {
        dampen = crm_get_msec(value);
        crm_trace("Created attribute %s with delay %dms (%s)", a->id, dampen, value);
    } else {
        crm_trace("Created attribute %s with no delay", a->id);
    }

    if(dampen > 0) {
        a->timeout_ms = dampen;
        a->timer = mainloop_timer_add(a->id, a->timeout_ms, FALSE, attribute_timer_cb, a);
    }

    g_hash_table_replace(attributes, a->id, a);
    return a;
}

/*!
 * \internal
 * \brief Respond to a client peer-remove request (i.e. propagate to all peers)
 *
 * \param[in] client_name Name of client that made request (for log messages)
 * \param[in] xml         Root of request XML
 *
 * \return void
 */
void
attrd_client_peer_remove(const char *client_name, xmlNode *xml)
{
    const char *host = crm_element_value(xml, F_ATTRD_HOST);

    if (host) {
        crm_info("Client %s is requesting all values for %s be removed",
                 client_name, host);
        send_attrd_message(NULL, xml); /* ends up at attrd_peer_message() */
    } else {
        crm_info("Ignoring request by client %s to remove all peer values without specifying peer",
                 client_name);
    }
}

/*!
 * \internal
 * \brief Respond to a client update request
 *
 * \param[in] xml         Root of request XML
 *
 * \return void
 */
void
attrd_client_update(xmlNode *xml)
{
    attribute_t *a = NULL;
    attribute_value_t *v = NULL;
    char *key = crm_element_value_copy(xml, F_ATTRD_KEY);
    char *set = crm_element_value_copy(xml, F_ATTRD_SET);
    char *host = crm_element_value_copy(xml, F_ATTRD_HOST);
    const char *attr = crm_element_value(xml, F_ATTRD_ATTRIBUTE);
    const char *value = crm_element_value(xml, F_ATTRD_VALUE);
    const char *regex = crm_element_value(xml, F_ATTRD_REGEX);

    /* If a regex was specified, broadcast a message for each match */
    if ((attr == NULL) && regex) {
        GHashTableIter aIter;
        regex_t *r_patt = calloc(1, sizeof(regex_t));

        crm_debug("Setting %s to %s", regex, value);
        if (regcomp(r_patt, regex, REG_EXTENDED)) {
            crm_err("Bad regex '%s' for update", regex);

        } else {
            g_hash_table_iter_init(&aIter, attributes);
            while (g_hash_table_iter_next(&aIter, (gpointer *) & attr, NULL)) {
                int status = regexec(r_patt, attr, 0, NULL, 0);

                if (status == 0) {
                    crm_trace("Matched %s with %s", attr, regex);
                    crm_xml_add(xml, F_ATTRD_ATTRIBUTE, attr);
                    send_attrd_message(NULL, xml);
                }
            }
        }

        free(key);
        free(set);
        free(host);
        regfree(r_patt);
        free(r_patt);
        return;
    }

    if (host == NULL) {
        crm_trace("Inferring host");
        host = strdup(attrd_cluster->uname);
        crm_xml_add(xml, F_ATTRD_HOST, host);
        crm_xml_add_int(xml, F_ATTRD_HOST_ID, attrd_cluster->nodeid);
    }

    a = g_hash_table_lookup(attributes, attr);

    /* If value was specified using ++ or += notation, expand to real value */
    if (value) {
        int offset = 1;
        int int_value = 0;
        static const int plus_plus_len = 5;

        if ((strlen(value) >= (plus_plus_len + 2)) && (value[plus_plus_len] == '+')
            && ((value[plus_plus_len + 1] == '+') || (value[plus_plus_len + 1] == '='))) {

            if (a) {
                v = g_hash_table_lookup(a->values, host);
            }
            if (v) {
                int_value = char2score(v->current);
            }

            if (value[plus_plus_len + 1] != '+') {
                const char *offset_s = value + (plus_plus_len + 2);

                offset = char2score(offset_s);
            }
            int_value += offset;

            if (int_value > INFINITY) {
                int_value = INFINITY;
            }

            crm_info("Expanded %s=%s to %d", attr, value, int_value);
            crm_xml_add_int(xml, F_ATTRD_VALUE, int_value);

            /* Replacing the value frees the previous memory, so re-query it */
            value = crm_element_value(xml, F_ATTRD_VALUE);
        }
    }

    if ((peer_writer == NULL) && (election_state(writer) != election_in_progress)) {
        crm_info("Starting an election to determine the writer");
        election_vote(writer);
    }

    crm_debug("Broadcasting %s[%s] = %s%s", attr, host, value,
              ((election_state(writer) == election_won)? " (writer)" : ""));

    free(key);
    free(set);
    free(host);

    send_attrd_message(NULL, xml); /* ends up at attrd_peer_message() */
}

/*!
 * \internal
 * \brief Respond to a client refresh request (i.e. write out all attributes)
 *
 * \return void
 */
void
attrd_client_refresh(void)
{
    GHashTableIter iter;
    attribute_t *a = NULL;

    /* 'refresh' forces a write of the current value of all attributes
     * Cancel any existing timers, we're writing it NOW
     */
    g_hash_table_iter_init(&iter, attributes);
    while (g_hash_table_iter_next(&iter, NULL, (gpointer *) & a)) {
        mainloop_timer_stop(a->timer);
    }

    crm_info("Updating all attributes");
    write_attributes(TRUE, FALSE);
}

/*!
 * \internal
 * \brief Build the XML reply to a client query
 *
 * param[in] attr Name of requested attribute
 * param[in] host Name of requested host (or NULL for all hosts)
 *
 * \return New XML reply
 * \note Caller is responsible for freeing the resulting XML
 */
static xmlNode *build_query_reply(const char *attr, const char *host)
{
    xmlNode *reply = create_xml_node(NULL, __FUNCTION__);
    attribute_t *a;

    if (reply == NULL) {
        return NULL;
    }
    crm_xml_add(reply, F_TYPE, T_ATTRD);
    crm_xml_add(reply, F_ATTRD_VERSION, ATTRD_PROTOCOL_VERSION);

    /* If desired attribute exists, add its value(s) to the reply */
    a = g_hash_table_lookup(attributes, attr);
    if (a) {
        attribute_value_t *v;
        xmlNode *host_value;

        crm_xml_add(reply, F_ATTRD_ATTRIBUTE, attr);

        /* Allow caller to use "localhost" to refer to local node */
        if (safe_str_eq(host, "localhost")) {
            host = attrd_cluster->uname;
            crm_trace("Mapped localhost to %s", host);
        }

        /* If a specific node was requested, add its value */
        if (host) {
            v = g_hash_table_lookup(a->values, host);
            host_value = create_xml_node(reply, XML_CIB_TAG_NODE);
            if (host_value == NULL) {
                free_xml(reply);
                return NULL;
            }
            crm_xml_add(host_value, F_ATTRD_HOST, host);
            crm_xml_add(host_value, F_ATTRD_VALUE, (v? v->current : NULL));

        /* Otherwise, add all nodes' values */
        } else {
            GHashTableIter iter;

            g_hash_table_iter_init(&iter, a->values);
            while (g_hash_table_iter_next(&iter, NULL, (gpointer *) &v)) {
                host_value = create_xml_node(reply, XML_CIB_TAG_NODE);
                if (host_value == NULL) {
                    free_xml(reply);
                    return NULL;
                }
                crm_xml_add(host_value, F_ATTRD_HOST, v->nodename);
                crm_xml_add(host_value, F_ATTRD_VALUE, v->current);
            }
        }
    }
    return reply;
}

/*!
 * \internal
 * \brief Respond to a client query
 *
 * \param[in] client Who queried us
 * \param[in] query  Root of query XML
 *
 * \return void
 */
void
attrd_client_query(crm_client_t *client, uint32_t id, uint32_t flags, xmlNode *query)
{
    const char *attr;
    const char *origin = crm_element_value(query, F_ORIG);
    ssize_t rc;
    xmlNode *reply;

    if (origin == NULL) {
        origin = "unknown client";
    }
    crm_debug("Query arrived from %s", origin);

    /* Request must specify attribute name to query */
    attr = crm_element_value(query, F_ATTRD_ATTRIBUTE);
    if (attr == NULL) {
        crm_warn("Ignoring malformed query from %s (no attribute name given)",
                 origin);
        return;
    }

    /* Build the XML reply */
    reply = build_query_reply(attr, crm_element_value(query, F_ATTRD_HOST));
    if (reply == NULL) {
        crm_err("Could not respond to query from %s: could not create XML reply",
                 origin);
        return;
    }
    crm_log_xml_trace(reply, "Reply");

    /* Send the reply to the client */
    client->request_id = 0;
    if ((rc = crm_ipcs_send(client, id, reply, flags)) < 0) {
        crm_err("Could not respond to query from %s: %s (%d)",
                origin, pcmk_strerror(-rc), -rc);
    }
    free_xml(reply);
}

void
attrd_peer_message(crm_node_t *peer, xmlNode *xml)
{
    int peer_state = 0;
    const char *v = crm_element_value(xml, F_ATTRD_VERSION);
    const char *op = crm_element_value(xml, F_ATTRD_TASK);
    const char *election_op = crm_element_value(xml, F_CRM_TASK);
    const char *host = crm_element_value(xml, F_ATTRD_HOST);

    if(election_op) {
        enum election_result rc = 0;

        crm_xml_add(xml, F_CRM_HOST_FROM, peer->uname);
        rc = election_count_vote(writer, xml, TRUE);
        switch(rc) {
            case election_start:
                free(peer_writer);
                peer_writer = NULL;
                election_vote(writer);
                break;
            case election_lost:
                free(peer_writer);
                peer_writer = strdup(peer->uname);
                break;
            default:
                election_check(writer);
                break;
        }
        return;

    } else if(v == NULL) {
        /* From the non-atomic version */
        if (safe_str_eq(op, ATTRD_OP_UPDATE)) {
            const char *name = crm_element_value(xml, F_ATTRD_ATTRIBUTE);

            crm_trace("Compatibility update of %s from %s", name, peer->uname);
            attrd_peer_update(peer, xml, host, FALSE);

        } else if (safe_str_eq(op, ATTRD_OP_FLUSH)) {
            const char *name = crm_element_value(xml, F_ATTRD_ATTRIBUTE);
            attribute_t *a = g_hash_table_lookup(attributes, name);

            if(a) {
                crm_trace("Compatibility write-out of %s for %s from %s", a->id, op, peer->uname);
                write_or_elect_attribute(a);
            }

        } else if (safe_str_eq(op, ATTRD_OP_REFRESH)) {
            GHashTableIter aIter;
            attribute_t *a = NULL;

            g_hash_table_iter_init(&aIter, attributes);
            while (g_hash_table_iter_next(&aIter, NULL, (gpointer *) & a)) {
                crm_trace("Compatibility write-out of %s for %s from %s", a->id, op, peer->uname);
                write_or_elect_attribute(a);
            }
        }
    }

    crm_element_value_int(xml, F_ATTRD_WRITER, &peer_state);
    if(election_state(writer) == election_won
       && peer_state == election_won
       && safe_str_neq(peer->uname, attrd_cluster->uname)) {
        crm_notice("Detected another attribute writer: %s", peer->uname);
        election_vote(writer);

    } else if(peer_state == election_won) {
        if(peer_writer == NULL) {
            peer_writer = strdup(peer->uname);
            crm_notice("Recorded attribute writer: %s", peer->uname);

        } else if(safe_str_neq(peer->uname, peer_writer)) {
            crm_notice("Recorded new attribute writer: %s (was %s)", peer->uname, peer_writer);
            free(peer_writer);
            peer_writer = strdup(peer->uname);
        }
    }

    if (safe_str_eq(op, ATTRD_OP_UPDATE)) {
        attrd_peer_update(peer, xml, host, FALSE);

    } else if (safe_str_eq(op, ATTRD_OP_SYNC)) {
        attrd_peer_sync(peer, xml);

    } else if (safe_str_eq(op, ATTRD_OP_PEER_REMOVE)) {
        int host_id = 0;
        char *endptr = NULL;

        host_id = strtol(host, &endptr, 10);
        if (errno != 0 || endptr == host || *endptr != '\0') {
            host_id = 0;
        } else {
            host = NULL;
        }
        attrd_peer_remove(host_id, host, TRUE, peer->uname);


    } else if (safe_str_eq(op, ATTRD_OP_SYNC_RESPONSE)
              && safe_str_neq(peer->uname, attrd_cluster->uname)) {
        xmlNode *child = NULL;

        crm_notice("Processing %s from %s", op, peer->uname);
        for (child = __xml_first_child(xml); child != NULL; child = __xml_next(child)) {
            host = crm_element_value(child, F_ATTRD_HOST);
            attrd_peer_update(peer, child, host, TRUE);
        }
    }
}

void
attrd_peer_sync(crm_node_t *peer, xmlNode *xml)
{
    GHashTableIter aIter;
    GHashTableIter vIter;

    attribute_t *a = NULL;
    attribute_value_t *v = NULL;
    xmlNode *sync = create_xml_node(NULL, __FUNCTION__);

    crm_xml_add(sync, F_ATTRD_TASK, ATTRD_OP_SYNC_RESPONSE);

    g_hash_table_iter_init(&aIter, attributes);
    while (g_hash_table_iter_next(&aIter, NULL, (gpointer *) & a)) {
        g_hash_table_iter_init(&vIter, a->values);
        while (g_hash_table_iter_next(&vIter, NULL, (gpointer *) & v)) {
            crm_debug("Syncing %s[%s] = %s to %s", a->id, v->nodename, v->current, peer?peer->uname:"everyone");
            build_attribute_xml(sync, a->id, a->set, a->uuid, a->timeout_ms, a->user, a->is_private,
                                v->nodename, v->nodeid, v->current);
        }
    }

    crm_debug("Syncing values to %s", peer?peer->uname:"everyone");
    send_attrd_message(peer, sync);
    free_xml(sync);
}

void
attrd_peer_remove(uint32_t nodeid, const char *host, gboolean uncache, const char *source)
{
    attribute_t *a = NULL;
    GHashTableIter aIter;

    crm_notice("Removing all %s attributes for %s", host, source);
    if(host == NULL) {
        return;
    }

    g_hash_table_iter_init(&aIter, attributes);
    while (g_hash_table_iter_next(&aIter, NULL, (gpointer *) & a)) {
        if(g_hash_table_remove(a->values, host)) {
            crm_debug("Removed %s[%s] for %s", a->id, host, source);
        }
    }

    if (uncache) {
        crm_remote_peer_cache_remove(host);
        reap_crm_member(nodeid, host);
    }
}

/*!
 * \internal
 * \brief Return host's hash table entry (creating one if needed)
 *
 * \param[in] values Hash table of values
 * \param[in] host Name of peer to look up
 * \param[in] xml XML describing the attribute
 *
 * \return Pointer to new or existing hash table entry
 */
static attribute_value_t *
attrd_lookup_or_create_value(GHashTable *values, const char *host, xmlNode *xml)
{
    attribute_value_t *v = g_hash_table_lookup(values, host);

    if (v == NULL) {
        v = calloc(1, sizeof(attribute_value_t));
        CRM_ASSERT(v != NULL);

        v->nodename = strdup(host);
        CRM_ASSERT(v->nodename != NULL);

        crm_element_value_int(xml, F_ATTRD_IS_REMOTE, &v->is_remote);
        if (v->is_remote == TRUE) {
            crm_remote_peer_cache_add(host);
        }

        g_hash_table_replace(values, v->nodename, v);
    }
    return(v);
}

void
attrd_peer_update(crm_node_t *peer, xmlNode *xml, const char *host, bool filter)
{
    bool changed = FALSE;
    attribute_value_t *v = NULL;

    const char *attr = crm_element_value(xml, F_ATTRD_ATTRIBUTE);
    const char *value = crm_element_value(xml, F_ATTRD_VALUE);

    attribute_t *a = g_hash_table_lookup(attributes, attr);

    if(a == NULL) {
        a = create_attribute(xml);
    }

    if(host == NULL) {
        GHashTableIter vIter;
        g_hash_table_iter_init(&vIter, a->values);

        crm_debug("Setting %s for all hosts to %s", attr, value);

        xml_remove_prop(xml, F_ATTRD_HOST_ID);
        while (g_hash_table_iter_next(&vIter, (gpointer *) & host, NULL)) {
            attrd_peer_update(peer, xml, host, filter);
        }
        return;
    }

    v = attrd_lookup_or_create_value(a->values, host, xml);

    if(filter
              && safe_str_neq(v->current, value)
              && safe_str_eq(host, attrd_cluster->uname)) {
        xmlNode *sync = create_xml_node(NULL, __FUNCTION__);
        crm_notice("%s[%s]: local value '%s' takes priority over '%s' from %s",
                   a->id, host, v->current, value, peer->uname);

        crm_xml_add(sync, F_ATTRD_TASK, ATTRD_OP_SYNC_RESPONSE);
        v = g_hash_table_lookup(a->values, host);
        build_attribute_xml(sync, a->id, a->set, a->uuid, a->timeout_ms, a->user, a->is_private,
                            v->nodename, v->nodeid, v->current);

        crm_xml_add_int(sync, F_ATTRD_WRITER, election_state(writer));
        send_attrd_message(peer, sync);
        free_xml(sync);

    } else if(safe_str_neq(v->current, value)) {
        crm_info("Setting %s[%s]: %s -> %s from %s", attr, host, v->current, value, peer->uname);
        free(v->current);
        if(value) {
            v->current = strdup(value);
        } else {
            v->current = NULL;
        }
        changed = TRUE;

    } else {
        crm_trace("Unchanged %s[%s] from %s is %s", attr, host, peer->uname, value);
    }

    a->changed |= changed;

    if(changed) {
        if(a->timer) {
            crm_trace("Delayed write out (%dms) for %s", a->timeout_ms, a->id);
            mainloop_timer_start(a->timer);
        } else {
            write_or_elect_attribute(a);
        }
    }

    /* this only involves cluster nodes. */
    if(v->nodeid == 0 && (v->is_remote == FALSE)) {
        if(crm_element_value_int(xml, F_ATTRD_HOST_ID, (int*)&v->nodeid) == 0) {
            /* Create the name/id association */
            crm_node_t *peer = crm_get_peer(v->nodeid, host);
            crm_trace("We know %s's node id now: %s", peer->uname, peer->uuid);
            if(election_state(writer) == election_won) {
                write_attributes(FALSE, TRUE);
                return;
            }
        }
    }
}

void
write_or_elect_attribute(attribute_t *a)
{
    enum election_result rc = election_state(writer);
    if(rc == election_won) {
        write_attribute(a);

    } else if(rc == election_in_progress) {
        crm_trace("Election in progress to determine who will write out %s", a->id);

    } else if(peer_writer == NULL) {
        crm_info("Starting an election to determine who will write out %s", a->id);
        election_vote(writer);

    } else {
        crm_trace("%s will write out %s, we are in state %d", peer_writer, a->id, rc);
    }
}

gboolean
attrd_election_cb(gpointer user_data)
{
    crm_trace("Election complete");

    free(peer_writer);
    peer_writer = strdup(attrd_cluster->uname);

    /* Update the peers after an election */
    attrd_peer_sync(NULL, NULL);

    /* Update the CIB after an election */
    write_attributes(TRUE, FALSE);
    return FALSE;
}


void
attrd_peer_change_cb(enum crm_status_type kind, crm_node_t *peer, const void *data)
{
    if ((kind == crm_status_nstate) || (kind == crm_status_rstate)) {
        if (safe_str_eq(peer->state, CRM_NODE_MEMBER)) {
            /* If we're the writer, send new peers a list of all attributes
             * (unless it's a remote node, which doesn't run its own attrd)
             */
            if ((election_state(writer) == election_won)
                && !is_set(peer->flags, crm_remote_node)) {
                attrd_peer_sync(peer, NULL);
            }
        } else {
            /* Remove all attribute values associated with lost nodes */
            attrd_peer_remove(peer->id, peer->uname, FALSE, __FUNCTION__);
            if (peer_writer && safe_str_eq(peer->uname, peer_writer)) {
                free(peer_writer);
                peer_writer = NULL;
                crm_notice("Lost attribute writer %s", peer->uname);
            }
        }
    }
}

static void
attrd_cib_callback(xmlNode * msg, int call_id, int rc, xmlNode * output, void *user_data)
{
    int level = LOG_ERR;
    GHashTableIter iter;
    const char *peer = NULL;
    attribute_value_t *v = NULL;

    char *name = user_data;
    attribute_t *a = g_hash_table_lookup(attributes, name);

    if(a == NULL) {
        crm_info("Attribute %s no longer exists", name);
        goto done;
    }

    a->update = 0;
    if (rc == pcmk_ok && call_id < 0) {
        rc = call_id;
    }

    switch (rc) {
        case pcmk_ok:
            level = LOG_INFO;
            last_cib_op_done = call_id;
            break;
        case -pcmk_err_diff_failed:    /* When an attr changes while the CIB is syncing */
        case -ETIME:           /* When an attr changes while there is a DC election */
        case -ENXIO:           /* When an attr changes while the CIB is syncing a
                                *   newer config from a node that just came up
                                */
            level = LOG_WARNING;
            break;
    }

    do_crm_log(level, "Update %d for %s: %s (%d)", call_id, name, pcmk_strerror(rc), rc);

    g_hash_table_iter_init(&iter, a->values);
    while (g_hash_table_iter_next(&iter, (gpointer *) & peer, (gpointer *) & v)) {
        do_crm_log(level, "Update %d for %s[%s]=%s: %s (%d)", call_id, a->id, peer, v->requested, pcmk_strerror(rc), rc);

        if(rc == pcmk_ok) {
            free(v->stored);
            v->stored = v->requested;
            v->requested = NULL;

        } else {
            free(v->requested);
            v->requested = NULL;
            a->changed = TRUE; /* Attempt write out again */
        }
    }
  done:
    if(a && a->changed && election_state(writer) == election_won) {
        write_attribute(a);
    }
}

void
write_attributes(bool all, bool peer_discovered)
{
    GHashTableIter iter;
    attribute_t *a = NULL;

    g_hash_table_iter_init(&iter, attributes);
    while (g_hash_table_iter_next(&iter, NULL, (gpointer *) & a)) {
        if (peer_discovered && a->unknown_peer_uuids) {
            /* a new peer uuid has been discovered, try writing this attribute again. */
            a->changed = TRUE;
        }

        if(all || a->changed) {
            write_attribute(a);
        } else {
            crm_debug("Skipping unchanged attribute %s", a->id);
        }
    }
}

static void
build_update_element(xmlNode *parent, attribute_t *a, const char *nodeid, const char *value)
{
    char *set = NULL;
    char *uuid = NULL;
    xmlNode *xml_obj = NULL;

    if(a->set) {
        set = strdup(a->set);
    } else {
        set = crm_strdup_printf("%s-%s", XML_CIB_TAG_STATUS, nodeid);
    }

    if(a->uuid) {
        uuid = strdup(a->uuid);
    } else {
        int lpc;
        uuid = crm_strdup_printf("%s-%s", set, a->id);

        /* Minimal attempt at sanitizing automatic IDs */
        for (lpc = 0; uuid[lpc] != 0; lpc++) {
            switch (uuid[lpc]) {
                case ':':
                    uuid[lpc] = '.';
            }
        }
    }

    xml_obj = create_xml_node(parent, XML_CIB_TAG_STATE);
    crm_xml_add(xml_obj, XML_ATTR_ID, nodeid);

    xml_obj = create_xml_node(xml_obj, XML_TAG_TRANSIENT_NODEATTRS);
    crm_xml_add(xml_obj, XML_ATTR_ID, nodeid);

    xml_obj = create_xml_node(xml_obj, XML_TAG_ATTR_SETS);
    crm_xml_add(xml_obj, XML_ATTR_ID, set);

    xml_obj = create_xml_node(xml_obj, XML_CIB_TAG_NVPAIR);
    crm_xml_add(xml_obj, XML_ATTR_ID, uuid);
    crm_xml_add(xml_obj, XML_NVPAIR_ATTR_NAME, a->id);

    if(value) {
        crm_xml_add(xml_obj, XML_NVPAIR_ATTR_VALUE, value);

    } else {
        crm_xml_add(xml_obj, XML_NVPAIR_ATTR_VALUE, "");
        crm_xml_add(xml_obj, "__delete__", XML_NVPAIR_ATTR_VALUE);
    }

    free(uuid);
    free(set);
}

void
write_attribute(attribute_t *a)
{
    int private_updates = 0, cib_updates = 0;
    xmlNode *xml_top = NULL;
    attribute_value_t *v = NULL;
    GHashTableIter iter;
    enum cib_call_options flags = cib_quorum_override;

    if (a == NULL) {
        return;
    }

    /* If this attribute will be written to the CIB ... */
    if (!a->is_private) {

        /* Defer the write if now's not a good time */
        if (the_cib == NULL) {
            crm_info("Write out of '%s' delayed: cib not connected", a->id);
            return;

        } else if (a->update && (a->update < last_cib_op_done)) {
            crm_info("Write out of '%s' continuing: update %d considered lost", a->id, a->update);

        } else if (a->update) {
            crm_info("Write out of '%s' delayed: update %d in progress", a->id, a->update);
            return;

        } else if (mainloop_timer_running(a->timer)) {
            crm_info("Write out of '%s' delayed: timer is running", a->id);
            return;
        }

        /* Initialize the status update XML */
        xml_top = create_xml_node(NULL, XML_CIB_TAG_STATUS);
    }

    /* Attribute will be written shortly, so clear changed flag */
    a->changed = FALSE;

    /* We will check all peers' uuids shortly, so initialize this to false */
    a->unknown_peer_uuids = FALSE;

    /* Iterate over each peer value of this attribute */
    g_hash_table_iter_init(&iter, a->values);
    while (g_hash_table_iter_next(&iter, NULL, (gpointer *) & v)) {
        crm_node_t *peer = crm_get_peer_full(v->nodeid, v->nodename, CRM_GET_PEER_ANY);

        /* If the value's peer info does not correspond to a peer, ignore it */
        if (peer == NULL) {
            crm_notice("Update error (peer not found): %s[%s]=%s failed (host=%p)",
                       v->nodename, a->id, v->current, peer);
            continue;
        }

        /* If we're just learning the peer's node id, remember it */
        if (peer->id && (v->nodeid == 0)) {
            crm_trace("Updating value's nodeid");
            v->nodeid = peer->id;
        }

        /* If this is a private attribute, no update needs to be sent */
        if (a->is_private) {
            private_updates++;
            continue;
        }

        /* If the peer is found, but its uuid is unknown, defer write */
        if (peer->uuid == NULL) {
            a->unknown_peer_uuids = TRUE;
            crm_notice("Update error (unknown peer uuid, retry will be attempted once uuid is discovered): %s[%s]=%s failed (host=%p)",
                       v->nodename, a->id, v->current, peer);
            continue;
        }

        /* Add this value to status update XML */
        crm_debug("Update: %s[%s]=%s (%s %u %u %s)", v->nodename, a->id,
                  v->current, peer->uuid, peer->id, v->nodeid, peer->uname);
        build_update_element(xml_top, a, peer->uuid, v->current);
        cib_updates++;

        free(v->requested);
        v->requested = NULL;
        if (v->current) {
            v->requested = strdup(v->current);
        } else {
            /* Older attrd versions don't know about the cib_mixed_update
             * flag so make sure it goes to the local cib which does
             */
            flags |= cib_mixed_update|cib_scope_local;
        }
    }

    if (private_updates) {
        crm_info("Processed %d private change%s for %s, id=%s, set=%s",
                 private_updates, ((private_updates == 1)? "" : "s"),
                 a->id, (a->uuid? a->uuid : "<n/a>"), a->set);
    }
    if (cib_updates) {
        crm_log_xml_trace(xml_top, __FUNCTION__);

        a->update = cib_internal_op(the_cib, CIB_OP_MODIFY, NULL, XML_CIB_TAG_STATUS, xml_top, NULL,
                                    flags, a->user);

        crm_info("Sent update %d with %d changes for %s, id=%s, set=%s",
                 a->update, cib_updates, a->id, (a->uuid? a->uuid : "<n/a>"), a->set);

        the_cib->cmds->register_callback_full(the_cib, a->update, 120, FALSE,
                                              strdup(a->id),
                                              "attrd_cib_callback",
                                              attrd_cib_callback, free);
    }
    free_xml(xml_top);
}
