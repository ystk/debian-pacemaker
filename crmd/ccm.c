/* 
 * Copyright (C) 2004 Andrew Beekhof <andrew@beekhof.net>
 * 
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 * 
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

/* put these first so that uuid_t is defined without conflicts */
#include <crm_internal.h>

#if SUPPORT_HEARTBEAT
#include <ocf/oc_event.h>
#include <ocf/oc_membership.h>
#endif

#include <string.h>

#include <crm/crm.h>
#include <crm/cib.h>
#include <crm/msg_xml.h>
#include <crm/common/xml.h>
#include <crm/common/cluster.h>
#include <crmd_messages.h>
#include <crmd_fsa.h>
#include <fsa_proto.h>
#include <crmd_callbacks.h>

gboolean membership_flux_hack = FALSE;
void post_cache_update(int instance);

#if SUPPORT_HEARTBEAT
oc_ev_t *fsa_ev_token;
void oc_ev_special(const oc_ev_t *, oc_ev_class_t , int );

void crmd_ccm_msg_callback(
    oc_ed_t event, void *cookie, size_t size, const void *data);
#endif

void ghash_update_cib_node(gpointer key, gpointer value, gpointer user_data);
void check_dead_member(const char *uname, GHashTable *members);
void reap_dead_ccm_nodes(gpointer key, gpointer value, gpointer user_data);

#define CCM_EVENT_DETAIL 0
#define CCM_EVENT_DETAIL_PARTIAL 0

int num_ccm_register_fails = 0;
int max_ccm_register_fails = 30;
int last_peer_update = 0;

extern GHashTable *voted;

struct update_data_s
{
		const char *state;
		const char *caller;
		xmlNode *updates;
		gboolean    overwrite_join;
};

void reap_dead_ccm_nodes(gpointer key, gpointer value, gpointer user_data)
{
    crm_node_t *node = value;
    if(crm_is_member_active(node) == FALSE) {
	check_dead_member(node->uname, NULL);
    }
}

extern gboolean check_join_state(enum crmd_fsa_state cur_state, const char *source);

void
check_dead_member(const char *uname, GHashTable *members)
{
	CRM_CHECK(uname != NULL, return);
	if(members != NULL && g_hash_table_lookup(members, uname) != NULL) {
		crm_err("%s didnt really leave the membership!", uname);
		return;
	}
	erase_node_from_join(uname);
	if(voted != NULL) {
		g_hash_table_remove(voted, uname);
	}
	
	if(safe_str_eq(fsa_our_uname, uname)) {
		crm_err("We're not part of the cluster anymore");
		register_fsa_input(C_FSA_INTERNAL, I_ERROR, NULL);

	} else if(AM_I_DC == FALSE && safe_str_eq(uname, fsa_our_dc)) {
		crm_warn("Our DC node (%s) left the cluster", uname);
		register_fsa_input(C_FSA_INTERNAL, I_ELECTION, NULL);

	} else if(fsa_state == S_INTEGRATION || fsa_state == S_FINALIZE_JOIN) {
	    check_join_state(fsa_state, __FUNCTION__);
	}    
}

/*	 A_CCM_CONNECT	*/
void
do_ccm_control(long long action,
		enum crmd_fsa_cause cause,
		enum crmd_fsa_state cur_state,
		enum crmd_fsa_input current_input,
		fsa_data_t *msg_data)
{	
#if SUPPORT_HEARTBEAT
    if(is_heartbeat_cluster()) {
	if(action & A_CCM_DISCONNECT){
		set_bit_inplace(fsa_input_register, R_CCM_DISCONNECTED);
		oc_ev_unregister(fsa_ev_token);
	}

	if(action & A_CCM_CONNECT) {
		int      ret;
		int	 fsa_ev_fd; 
		gboolean did_fail = FALSE;
		crm_debug_3("Registering with CCM");
		clear_bit_inplace(fsa_input_register, R_CCM_DISCONNECTED);
		ret = oc_ev_register(&fsa_ev_token);
		if (ret != 0) {
			crm_warn("CCM registration failed");
			did_fail = TRUE;
		}

		if(did_fail == FALSE) {
			crm_debug_3("Setting up CCM callbacks");
			ret = oc_ev_set_callback(fsa_ev_token, OC_EV_MEMB_CLASS,
						 crmd_ccm_msg_callback, NULL);
			if (ret != 0) {
				crm_warn("CCM callback not set");
				did_fail = TRUE;
			}
		}
		if(did_fail == FALSE) {
			oc_ev_special(fsa_ev_token, OC_EV_MEMB_CLASS, 0/*don't care*/);
			
			crm_debug_3("Activating CCM token");
			ret = oc_ev_activate(fsa_ev_token, &fsa_ev_fd);
			if (ret != 0){
				crm_warn("CCM Activation failed");
				did_fail = TRUE;
			}
		}

		if(did_fail) {
			num_ccm_register_fails++;
			oc_ev_unregister(fsa_ev_token);
			
			if(num_ccm_register_fails < max_ccm_register_fails) {
				crm_warn("CCM Connection failed"
					 " %d times (%d max)",
					 num_ccm_register_fails,
					 max_ccm_register_fails);
				
				crm_timer_start(wait_timer);
				crmd_fsa_stall(NULL);
				return;
				
			} else {
				crm_err("CCM Activation failed %d (max) times",
					num_ccm_register_fails);
				register_fsa_error(C_FSA_INTERNAL, I_FAIL, NULL);
				return;
			}
		}
		

		crm_info("CCM connection established..."
			 " waiting for first callback");

		G_main_add_fd(G_PRIORITY_HIGH, fsa_ev_fd, FALSE, ccm_dispatch,
			      fsa_ev_token, default_ipc_connection_destroy);
		
	}
    }
#endif
    
    if(action & ~(A_CCM_CONNECT|A_CCM_DISCONNECT)) {
	crm_err("Unexpected action %s in %s",
		fsa_action2string(action), __FUNCTION__);
    }
}

#if SUPPORT_HEARTBEAT
void
ccm_event_detail(const oc_ev_membership_t *oc, oc_ed_t event)
{
	int lpc;
	gboolean member = FALSE;
	member = FALSE;

	crm_debug_2("-----------------------");
	crm_info("%s: trans=%d, nodes=%d, new=%d, lost=%d n_idx=%d, "
		 "new_idx=%d, old_idx=%d",
		 ccm_event_name(event),
		 oc->m_instance,
		 oc->m_n_member,
		 oc->m_n_in,
		 oc->m_n_out,
		 oc->m_memb_idx,
		 oc->m_in_idx,
		 oc->m_out_idx);
	
#if !CCM_EVENT_DETAIL_PARTIAL
	for(lpc=0; lpc < oc->m_n_member; lpc++) {
		crm_info("\tCURRENT: %s [nodeid=%d, born=%d]",
		       oc->m_array[oc->m_memb_idx+lpc].node_uname,
		       oc->m_array[oc->m_memb_idx+lpc].node_id,
		       oc->m_array[oc->m_memb_idx+lpc].node_born_on);

		if(safe_str_eq(fsa_our_uname,
			       oc->m_array[oc->m_memb_idx+lpc].node_uname)) {
			member = TRUE;
		}
	}
	if (member == FALSE) {
		crm_warn("MY NODE IS NOT IN CCM THE MEMBERSHIP LIST");
	}
#endif
	for(lpc=0; lpc<(int)oc->m_n_in; lpc++) {
		crm_info("\tNEW:     %s [nodeid=%d, born=%d]",
		       oc->m_array[oc->m_in_idx+lpc].node_uname,
		       oc->m_array[oc->m_in_idx+lpc].node_id,
		       oc->m_array[oc->m_in_idx+lpc].node_born_on);
	}
	
	for(lpc=0; lpc<(int)oc->m_n_out; lpc++) {
		crm_info("\tLOST:    %s [nodeid=%d, born=%d]",
		       oc->m_array[oc->m_out_idx+lpc].node_uname,
		       oc->m_array[oc->m_out_idx+lpc].node_id,
		       oc->m_array[oc->m_out_idx+lpc].node_born_on);
	}
	
	crm_debug_2("-----------------------");
	
}

#endif

gboolean ever_had_quorum = FALSE;

void
post_cache_update(int instance) 
{
    xmlNode *no_op = NULL;
    
    crm_peer_seq = instance;
    crm_debug("Updated cache after membership event %d.", instance);

    g_hash_table_foreach(crm_peer_cache, reap_dead_ccm_nodes, NULL);	
    set_bit_inplace(fsa_input_register, R_CCM_DATA);
    
    if(AM_I_DC) {
	populate_cib_nodes(FALSE);
	do_update_cib_nodes(FALSE, __FUNCTION__);
    }

    /*
     * If we lost nodes, we should re-check the election status
     * Safe to call outside of an election
     */
    register_fsa_action(A_ELECTION_CHECK);
    
    /* Membership changed, remind everyone we're here.
     * This will aid detection of duplicate DCs
     */
    no_op = create_request(
	CRM_OP_NOOP, NULL, NULL, CRM_SYSTEM_CRMD,
	AM_I_DC?CRM_SYSTEM_DC:CRM_SYSTEM_CRMD, NULL);
    send_cluster_message(NULL, crm_msg_crmd, no_op, FALSE);
    free_xml(no_op);
}


/*	 A_CCM_UPDATE_CACHE	*/
/*
 * Take the opportunity to update the node status in the CIB as well
 */
#if SUPPORT_HEARTBEAT
void
do_ccm_update_cache(
    enum crmd_fsa_cause cause, enum crmd_fsa_state cur_state,
    oc_ed_t event, const oc_ev_membership_t *oc, xmlNode *xml)
{
	unsigned long long instance = 0;
	unsigned int lpc = 0;
	
	if(is_heartbeat_cluster()) {
	    CRM_ASSERT(oc != NULL);
	    instance = oc->m_instance;
	}
	
	CRM_ASSERT(crm_peer_seq <= instance);

	switch(cur_state) {
	    case S_STOPPING:
	    case S_TERMINATE:
	    case S_HALT:
		crm_debug("Ignoring %s CCM event %llu, we're in state %s", 
			  ccm_event_name(event), instance,
			  fsa_state2string(cur_state));
		return;
	    case S_ELECTION:
		register_fsa_action(A_ELECTION_CHECK);
		break;
	    default:
		break;
	}
	
	if(is_heartbeat_cluster()) {
	    ccm_event_detail(oc, event);
	    
	/*--*-- Recently Dead Member Nodes --*--*/
	    for(lpc=0; lpc < oc->m_n_out; lpc++) {
		crm_update_ccm_node(oc, lpc+oc->m_out_idx, CRM_NODE_LOST, instance);
	    }
	    
	    /*--*-- All Member Nodes --*--*/
	    for(lpc=0; lpc < oc->m_n_member; lpc++) {
		crm_update_ccm_node(oc, lpc+oc->m_memb_idx, CRM_NODE_ACTIVE, instance);
	    }
	}

	if(event == OC_EV_MS_EVICTED) {
	    crm_update_peer(
		0, 0, 0, -1, 0,
		fsa_our_uuid, fsa_our_uname, NULL, CRM_NODE_EVICTED);

	    /* todo: drop back to S_PENDING instead */
	    /* get out... NOW!
	     *
	     * go via the error recovery process so that HA will
	     *    restart us if required
	     */
	    register_fsa_error_adv(cause, I_ERROR, NULL, NULL, __FUNCTION__);
	}

	post_cache_update(instance);
	return;
}
#endif

static void
ccm_node_update_complete(xmlNode *msg, int call_id, int rc,
			 xmlNode *output, void *user_data)
{
	fsa_data_t *msg_data = NULL;
	last_peer_update = 0;
	
	if(rc == cib_ok) {
		crm_debug_2("Node update %d complete", call_id);

	} else {
		crm_err("Node update %d failed", call_id);
		crm_log_xml(LOG_DEBUG, "failed", msg);
		register_fsa_error(C_FSA_INTERNAL, I_ERROR, NULL);
	}
}

void
ghash_update_cib_node(gpointer key, gpointer value, gpointer user_data)
{
    xmlNode *tmp1 = NULL;
    const char *join = NULL;
    crm_node_t *node = value;
    struct update_data_s* data = (struct update_data_s*)user_data;

    data->state = XML_BOOLEAN_NO;
    if(safe_str_eq(node->state, CRM_NODE_ACTIVE)) {
	data->state = XML_BOOLEAN_YES;
    }
    
    crm_debug("Updating %s: %s (overwrite=%s) hash_size=%d",
	      node->uname, data->state, data->overwrite_join?"true":"false",
	      g_hash_table_size(confirmed_nodes));
    
    if(data->overwrite_join) {
	if((node->processes & crm_proc_crmd) == FALSE) {
	    join = CRMD_JOINSTATE_DOWN;
	    
	} else {
	    const char *peer_member = g_hash_table_lookup(
		confirmed_nodes, node->uname);
	    if(peer_member != NULL) {
		join = CRMD_JOINSTATE_MEMBER;
	    } else {
		join = CRMD_JOINSTATE_PENDING;
	    }
	}
    }
    
    tmp1 = create_node_state(
	node->uname, (node->processes&crm_proc_ais)?ACTIVESTATUS:DEADSTATUS,
	data->state, (node->processes&crm_proc_crmd)?ONLINESTATUS:OFFLINESTATUS,
	join, NULL, FALSE, data->caller);
    
    add_node_copy(data->updates, tmp1);
    free_xml(tmp1);
}

void
do_update_cib_nodes(gboolean overwrite, const char *caller)
{
    int call_id = 0;
    int call_options = cib_scope_local|cib_quorum_override;
    struct update_data_s update_data;
    xmlNode *fragment = NULL;
    
    if(crm_peer_cache == NULL) {
	/* We got a replace notification before being connected to
	 *   the CCM.
	 * So there is no need to update the local CIB with our values
	 *   - since we have none.
	 */
	return;
	
    } else if(AM_I_DC == FALSE) {
	return;
    }
    
    fragment = create_xml_node(NULL, XML_CIB_TAG_STATUS);
    
    update_data.caller = caller;
    update_data.updates = fragment;
    update_data.overwrite_join = overwrite;
    
    g_hash_table_foreach(crm_peer_cache, ghash_update_cib_node, &update_data);
    
    fsa_cib_update(XML_CIB_TAG_STATUS, fragment, call_options, call_id);
    add_cib_op_callback(fsa_cib_conn, call_id, FALSE, NULL, ccm_node_update_complete);
    last_peer_update = call_id;
    
    free_xml(fragment);
}

static void cib_quorum_update_complete(
    xmlNode *msg, int call_id, int rc, xmlNode *output, void *user_data)
{
	fsa_data_t *msg_data = NULL;
	
	if(rc == cib_ok) {
		crm_debug_2("Quorum update %d complete", call_id);

	} else {
		crm_err("Quorum update %d failed", call_id);
		crm_log_xml(LOG_DEBUG, "failed", msg);
		register_fsa_error(C_FSA_INTERNAL, I_ERROR, NULL);
	}
} 

void crm_update_quorum(gboolean quorum, gboolean force_update) 
{
    ever_had_quorum |= quorum;
    if(AM_I_DC && (force_update || fsa_has_quorum != quorum)) {
	int call_id = 0;
	xmlNode *update = NULL;
	int call_options = cib_scope_local|cib_quorum_override;
	
	update = create_xml_node(NULL, XML_TAG_CIB);
	crm_xml_add_int(update, XML_ATTR_HAVE_QUORUM, quorum);
	set_uuid(update, XML_ATTR_DC_UUID, fsa_our_uname);
	
	fsa_cib_update(XML_TAG_CIB, update, call_options, call_id);
	crm_info("Updating quorum status to %s (call=%d)", quorum?"true":"false", call_id);
	add_cib_op_callback(fsa_cib_conn, call_id, FALSE, NULL, cib_quorum_update_complete);
	free_xml(update);
    }
    fsa_has_quorum = quorum;
}

