#include "../../tcp_public.h"
#include "isis_const.h"
#include "isis_pkt.h"
#include "isis_intf.h"
#include "isis_adjacency.h"
#include "isis_rtr.h"
#include "isis_events.h"
#include "isis_flood.h"
#include "isis_lspdb.h"
#include "isis_spf.h"

bool
isis_pkt_trap_rule(char *pkt, size_t pkt_size) {

    ethernet_hdr_t *eth_hdr = (ethernet_hdr_t *)pkt;

	if (eth_hdr->type == ISIS_ETH_PKT_TYPE) {
		return true;
	}
	return false;
}

static void
isis_process_hello_pkt(node_t *node,
                       interface_t *iif,
                       ethernet_hdr_t *hello_eth_hdr,
                       size_t pkt_size) {

    uint8_t intf_ip_len;
        
    if (!isis_node_intf_is_enable(iif)) return;

    /* Use the same fn for recv qualification as well */
    if (!isis_interface_qualify_to_send_hellos(iif)) {
        return;
    }
    
    /*Reject the pkt if dst mac is not Brodcast mac*/
    if(!IS_MAC_BROADCAST_ADDR(hello_eth_hdr->dst_mac.mac)){
        goto bad_hello;
	}

    /* Reject hello if ip_address in hello do not lies in same subnet as
     * reciepient interface*/

    byte *hello_pkt = (byte *)
        GET_ETHERNET_HDR_PAYLOAD(hello_eth_hdr);
    
    byte *hello_tlv_buffer =
        hello_pkt + sizeof(isis_pkt_type_t);
    
    size_t tlv_buff_size = pkt_size - \
                           ETH_HDR_SIZE_EXCL_PAYLOAD - \
                           sizeof(isis_pkt_type_t); 

    /*Fetch the IF IP Address Value from TLV buffer*/
    byte *if_ip_addr = tlv_buffer_get_particular_tlv(
                        hello_tlv_buffer, 
                        tlv_buff_size, 
                        ISIS_TLV_IF_IP, 
                        &intf_ip_len);

    /*If no Intf IP, then it is a bad hello*/
    if(!if_ip_addr) goto bad_hello;

    if(!is_same_subnet(IF_IP(iif), 
                       iif->intf_nw_props.mask, 
                       if_ip_addr)){

        isis_adjacency_t *adjacency = isis_find_adjacency_on_interface(iif, NULL);

        if (adjacency) {
            sprintf(tlb, "%s : Adjacency %s will be brought down, bad hello recvd\n",
                ISIS_ADJ_MGMT, isis_adjacency_name(adjacency));
            tcp_trace(iif->att_node, iif, tlb);
            isis_change_adjacency_state(adjacency, ISIS_ADJ_STATE_DOWN);
        }
        goto bad_hello;
    }
    isis_update_interface_adjacency_from_hello(iif, hello_tlv_buffer, tlv_buff_size);
    return ;

    bad_hello:
    ISIS_INCREMENT_STATS(iif, bad_hello_pkt_recvd);
}


static void
isis_process_lsp_pkt(node_t *node,
                     interface_t *iif,
                     ethernet_hdr_t *lsp_eth_hdr,
                     size_t pkt_size) {

    isis_pkt_t *new_lsp_pkt;
    isis_node_info_t *isis_node_info;
    
    if (!isis_node_intf_is_enable(iif)) return;
    
    if (!isis_any_adjacency_up_on_interface(iif)) return;

    if (isis_is_protocol_shutdown_in_progress(node)) return;

    ISIS_INCREMENT_STATS(iif, good_lsps_pkt_recvd);

    new_lsp_pkt = calloc(1, sizeof(isis_pkt_t));
    new_lsp_pkt->flood_eligibility = true;
    new_lsp_pkt->isis_pkt_type = ISIS_LSP_PKT_TYPE;
    new_lsp_pkt->pkt = tcp_ip_get_new_pkt_buffer(pkt_size);
    memcpy(new_lsp_pkt->pkt, (byte *)lsp_eth_hdr, pkt_size);
    new_lsp_pkt->pkt_size = pkt_size;

    isis_ref_isis_pkt(new_lsp_pkt);

    sprintf(tlb, "%s : lsp %s recvd on intf %s\n",
        ISIS_LSPDB_MGMT,
        isis_print_lsp_id(new_lsp_pkt), iif ? iif->if_name : 0);
    tcp_trace(node, iif, tlb);

    isis_install_lsp(node, iif, new_lsp_pkt);
    isis_deref_isis_pkt(new_lsp_pkt);
}

void
isis_pkt_recieve(void *arg, size_t arg_size) {

    node_t *node;
    interface_t *iif;
    uint32_t pkt_size;
    hdr_type_t hdr_code;
    ethernet_hdr_t *eth_hdr;
    pkt_notif_data_t *pkt_notif_data;
    isis_node_info_t *isis_node_info;

    pkt_notif_data = (pkt_notif_data_t *)arg;

    node        = pkt_notif_data->recv_node;
    iif         = pkt_notif_data->recv_interface;
    eth_hdr     = (ethernet_hdr_t *) pkt_notif_data->pkt;
    pkt_size    = pkt_notif_data->pkt_size;
	hdr_code    = pkt_notif_data->hdr_code;	

    if (hdr_code != ETH_HDR) return;
    
    if (!isis_is_protocol_enable_on_node(node)) {
        return;
    }

    char *isis_pkt_payload = (char *)GET_ETHERNET_HDR_PAYLOAD(eth_hdr);

    isis_pkt_type_t isis_pkt_type = ISIS_PKT_TYPE(isis_pkt_payload);

    switch(isis_pkt_type) {

        case ISIS_PTP_HELLO_PKT_TYPE:
            isis_process_hello_pkt(node, iif, eth_hdr, pkt_size); 
        break;
        case ISIS_LSP_PKT_TYPE:
            isis_process_lsp_pkt(node, iif, eth_hdr, pkt_size);
        break;
        default:; 
    }
}

static bool
isis_should_insert_on_demand_tlv(node_t *node, isis_event_type_t event_type) {

    isis_node_info_t *isis_node_info = ISIS_NODE_INFO(node);

    if (!isis_node_info->on_demand_flooding) {
        return false; 
    }

    switch(event_type) {

        case isis_event_adj_state_changed:
            return true;
        default:
            return false;
    }
}

static void
isis_create_fresh_lsp_pkt(node_t *node) {

    bool include_on_demand_tlv;
    size_t lsp_pkt_size_estimate = 0;
    bool is_proto_shutting_down;
    isis_pkt_hdr_t *lsp_pkt_hdr;
    bool create_purge_lsp;
    isis_node_info_t *isis_node_info = ISIS_NODE_INFO(node);

    create_purge_lsp = false;
    include_on_demand_tlv = false;
    is_proto_shutting_down = isis_is_protocol_shutdown_in_progress(node);

    /* Dont use the fn isis_is_protocol_enable_on_node( ) because
       we want to generate purge LSP when protocol is shutting down
    */
    if (!isis_node_info) return;

    /* Now estimate the size of lsp pkt */
    lsp_pkt_size_estimate += ETH_HDR_SIZE_EXCL_PAYLOAD;
    lsp_pkt_size_estimate += sizeof(isis_pkt_hdr_t);

    if (is_proto_shutting_down) {
        
        create_purge_lsp = true;
        goto TLV_ADD_DONE;
    }


    if (!is_proto_shutting_down) {
        /* TLVs */
        lsp_pkt_size_estimate += TLV_OVERHEAD_SIZE + NODE_NAME_SIZE; /* Device name */
        /* Nbr TLVs */
        lsp_pkt_size_estimate +=  isis_size_to_encode_all_nbr_tlv(node);
    }

    /* On Demand TLV size estimation */

    if (isis_is_reconciliation_in_progress(node) ||
        IS_BIT_SET(isis_node_info->event_control_flags,
                    ISIS_EVENT_ADMIN_ACTION_DB_CLEAR_BIT)) {

        include_on_demand_tlv = true;
    }

    if (include_on_demand_tlv) {
        lsp_pkt_size_estimate += TLV_OVERHEAD_SIZE + 1;
    }

    if (lsp_pkt_size_estimate > MAX_PACKET_BUFFER_SIZE) {
        return;
    }

TLV_ADD_DONE:

    /* Get rid of out-dated self lsp pkt */
    if (isis_node_info->self_lsp_pkt) {
        /* Debar this pkt from going out of the box*/
        isis_mark_isis_lsp_pkt_flood_ineligible(node, 
                isis_node_info->self_lsp_pkt);
        isis_deref_isis_pkt(isis_node_info->self_lsp_pkt);
        isis_node_info->self_lsp_pkt = NULL;
    }

    isis_node_info->seq_no++;

    ethernet_hdr_t *eth_hdr = (ethernet_hdr_t *)
                                tcp_ip_get_new_pkt_buffer(lsp_pkt_size_estimate);
    
    memset (eth_hdr->src_mac.mac, 0, sizeof(mac_add_t));
    layer2_fill_with_broadcast_mac(eth_hdr->dst_mac.mac);
    eth_hdr->type = ISIS_ETH_PKT_TYPE;

    lsp_pkt_hdr = (isis_pkt_hdr_t *)GET_ETHERNET_HDR_PAYLOAD(eth_hdr);

    /* pkt type */
    lsp_pkt_hdr->isis_pkt_type = ISIS_LSP_PKT_TYPE;
    lsp_pkt_hdr->seq_no = isis_node_info->seq_no;
    lsp_pkt_hdr->rtr_id = tcp_ip_covert_ip_p_to_n(NODE_LO_ADDR(node));
    
    if (create_purge_lsp) {
        SET_BIT(lsp_pkt_hdr->flags, ISIS_LSP_PKT_F_PURGE_BIT);
    }

    if (isis_is_overloaded(node, NULL)) {
        SET_BIT(lsp_pkt_hdr->flags, ISIS_LSP_PKT_F_OVERLOAD_BIT);
    }

    byte *lsp_tlv_buffer = (byte *)(lsp_pkt_hdr + 1);
    
    if (!is_proto_shutting_down) {

        /* Now TLV */
        lsp_tlv_buffer = tlv_buffer_insert_tlv(lsp_tlv_buffer,
                                        ISIS_TLV_HOSTNAME,
                                        NODE_NAME_SIZE, node->node_name);

        lsp_tlv_buffer = isis_encode_all_nbr_tlvs(node, lsp_tlv_buffer);

        /* On Demand TLV insertion */
        if (include_on_demand_tlv) {
            bool true_f = true;
            lsp_tlv_buffer = tlv_buffer_insert_tlv(lsp_tlv_buffer,
                                             ISIS_TLV_ON_DEMAND,
                                             1, (char *)&true_f);
        }
    }
    
    SET_COMMON_ETH_FCS(eth_hdr, lsp_pkt_size_estimate, 0);

    isis_node_info->self_lsp_pkt = calloc(1, sizeof(isis_pkt_t));
    isis_node_info->self_lsp_pkt->flood_eligibility = true;
    isis_node_info->self_lsp_pkt->isis_pkt_type = ISIS_LSP_PKT_TYPE;
    isis_node_info->self_lsp_pkt->pkt = (byte *)eth_hdr;
    isis_node_info->self_lsp_pkt->pkt_size = lsp_pkt_size_estimate;
    isis_node_info->self_lsp_pkt->ref_count = 1;
    UNSET_BIT64(isis_node_info->event_control_flags,
        ISIS_EVENT_ADMIN_ACTION_DB_CLEAR_BIT);
}

void
isis_generate_lsp_pkt(void *arg, uint32_t arg_size_unused) {

    node_t *node = (node_t *)arg;
    isis_node_info_t *isis_node_info = ISIS_NODE_INFO(node);

    isis_node_info->lsp_pkt_gen_task = NULL;

    sprintf(tlb, "%s : Self-LSP Generation task triggered\n",
            ISIS_LSPDB_MGMT);
    tcp_trace(node, 0 , tlb);

    /* Now generate LSP pkt */
    isis_create_fresh_lsp_pkt(node);

    isis_update_lsp_flood_timer_with_new_lsp_pkt(node,
        isis_node_info->self_lsp_pkt);
    
    isis_install_lsp(node, 0, isis_node_info->self_lsp_pkt);
}

void
isis_schedule_lsp_pkt_generation(node_t *node,
                isis_event_type_t event_type) {

    isis_node_info_t *isis_node_info = ISIS_NODE_INFO(node);

    if (!isis_node_info) return;

    if ( IS_BIT_SET (isis_node_info->misc_flags,
                ISIS_F_DISABLE_LSP_GEN)) {

        sprintf(tlb, "%s : LSP generation disabled", ISIS_LSPDB_MGMT);
        tcp_trace(node, 0, tlb);
        return;
    }

    if ( isis_is_protocol_shutdown_in_progress(node)) {
        /* Prevent any further LSP generation, we are interested only in 
                generating purge LSP one last time */
        SET_BIT(isis_node_info->misc_flags, ISIS_F_DISABLE_LSP_GEN);
    }

    SET_BIT( isis_node_info->event_control_flags, 
                        isis_event_to_event_bit(event_type));

    if (isis_node_info->lsp_pkt_gen_task) {

        sprintf(tlb, "%s : LSP generation Already scheduled, reason : %s\n",
            ISIS_LSPDB_MGMT,
            isis_event_str(event_type));
        tcp_trace(node, 0, tlb);
        return;
    }

    sprintf(tlb, "%s : LSP generation scheduled, reason : %s\n",
            ISIS_LSPDB_MGMT, isis_event_str(event_type));
    tcp_trace(node, 0, tlb);

    isis_node_info->lsp_pkt_gen_task =
        task_create_new_job(node, isis_generate_lsp_pkt, TASK_ONE_SHOT);
}

byte *
isis_prepare_hello_pkt(interface_t *intf, size_t *hello_pkt_size) {

    byte *temp;
    node_t *node;
    uint32_t four_byte_data;

    uint32_t eth_hdr_playload_size =
                sizeof(isis_pkt_type_t) +  /*ISIS pkt type code*/ 
                (TLV_OVERHEAD_SIZE * 6) + /*There shall be Six TLVs, hence 4 TLV overheads*/
                NODE_NAME_SIZE +    /* Data length of TLV: ISIS_TLV_NODE_NAME*/
                16 +                /* Data length of ISIS_TLV_RTR_NAME which is 16*/
                16 +                /* Data length of ISIS_TLV_IF_IP which is 16*/
                4  +                 /* Data length of ISIS_TLV_IF_INDEX which is 4*/
                4 +                  /* Data length for ISIS_ISIS_TLV_HOLD_TIME */
                4;                    /* Data length for ISIS_ISIS_TLV_METRIC_VAL */

    *hello_pkt_size = ETH_HDR_SIZE_EXCL_PAYLOAD + /*Dst Mac + Src mac + type field + FCS field*/
                      eth_hdr_playload_size;

    ethernet_hdr_t *hello_eth_hdr =
        (ethernet_hdr_t *)tcp_ip_get_new_pkt_buffer(*hello_pkt_size);

    memcpy(hello_eth_hdr->src_mac.mac, IF_MAC(intf), sizeof(mac_add_t));
    layer2_fill_with_broadcast_mac(hello_eth_hdr->dst_mac.mac);
    hello_eth_hdr->type = ISIS_ETH_PKT_TYPE;

    node = intf->att_node;
    temp = (char *)GET_ETHERNET_HDR_PAYLOAD(hello_eth_hdr);

    ISIS_PKT_TYPE(temp) = ISIS_PTP_HELLO_PKT_TYPE;
    
    temp = (temp + sizeof(isis_pkt_type_t));

    temp = tlv_buffer_insert_tlv(temp, ISIS_TLV_HOSTNAME, 
                                NODE_NAME_SIZE, node->node_name);
    temp = tlv_buffer_insert_tlv(temp, ISIS_TLV_RTR_ID,
                                16, NODE_LO_ADDR(node));
    temp = tlv_buffer_insert_tlv(temp, ISIS_TLV_IF_IP, 
                                16, IF_IP(intf));
    temp = tlv_buffer_insert_tlv(temp, ISIS_TLV_IF_INDEX,
                                 4,  (char *)&IF_INDEX(intf));

    four_byte_data = ISIS_INTF_HELLO_INTERVAL(intf) * ISIS_HOLD_TIME_FACTOR;

    temp = tlv_buffer_insert_tlv(temp, ISIS_TLV_HOLD_TIME,
                                 4, (char *)&four_byte_data);

    four_byte_data = ISIS_INTF_COST(intf);

    temp = tlv_buffer_insert_tlv(temp, ISIS_TLV_METRIC_VAL,
                                 4, (char *)&four_byte_data);

    SET_COMMON_ETH_FCS(hello_eth_hdr, eth_hdr_playload_size, 0);
    return (byte *)hello_eth_hdr;  
}

static void
isis_print_lsp_pkt(pkt_info_t *pkt_info ) {

    int rc = 0;
	char *buff;
	uint32_t pkt_size;
    uint16_t bytes_read = 0;
    unsigned char *ip_addr;

    byte tlv_type, tlv_len, *tlv_value = NULL;

	buff = pkt_info->pkt_print_buffer;
	pkt_size = pkt_info->pkt_size;

    isis_pkt_hdr_t* lsp_pkt_hdr = (isis_pkt_hdr_t *)(pkt_info->pkt);

	assert(pkt_info->protocol_no == ISIS_ETH_PKT_TYPE);

    rc = sprintf(buff + rc, "ISIS_LSP_PKT_TYPE : ");
    
    uint32_t seq_no = lsp_pkt_hdr->seq_no;
    uint32_t rtr_id = lsp_pkt_hdr->rtr_id;

    ip_addr = tcp_ip_covert_ip_n_to_p(rtr_id, 0);

    rc += sprintf(buff + rc, "LSP pkt : %s(%u) \n",
                    ip_addr, seq_no);

    byte *lsp_tlv_buffer = (byte *)(lsp_pkt_hdr + 1);

    uint16_t lsp_tlv_buffer_size = (uint16_t)(pkt_size - sizeof(isis_pkt_hdr_t));

    ITERATE_TLV_BEGIN(lsp_tlv_buffer, tlv_type,
                        tlv_len, tlv_value,
                        lsp_tlv_buffer_size) {

        switch(tlv_type) {
            case ISIS_TLV_HOSTNAME:
                rc += sprintf(buff + rc, "\tTLV%d Host-Name : %s\n", 
                        tlv_type, tlv_value);
            break;
            case ISIS_IS_REACH_TLV:
                rc += isis_print_formatted_nbr_tlv(buff + rc, 
                        tlv_value - TLV_OVERHEAD_SIZE,
                        tlv_len + TLV_OVERHEAD_SIZE);
                break;
            case ISIS_TLV_ON_DEMAND:
                rc += sprintf(buff + rc,"\tTLV%d On-Demand TLV : %hhu\n",
                        tlv_type, *(uint8_t *)tlv_value);
                break;
            default: ;
        }
    } ITERATE_TLV_END(lsp_tlv_buffer, tlv_type,
                        tlv_len, tlv_value,
                        lsp_tlv_buffer_size);

    pkt_info->bytes_written = rc;
}

static void
isis_print_hello_pkt(pkt_info_t *pkt_info ) {

    int rc = 0;
	char *buff;
	uint32_t pkt_size;

    byte tlv_type, tlv_len, *tlv_value = NULL;

	buff = pkt_info->pkt_print_buffer;
	pkt_size = pkt_info->pkt_size;

    byte* hpkt = (byte *)(pkt_info->pkt);

	assert(pkt_info->protocol_no == ISIS_ETH_PKT_TYPE);

    rc = sprintf(buff, "ISIS_PTP_HELLO_PKT_TYPE : ");

    ITERATE_TLV_BEGIN(((byte *)hpkt + sizeof(isis_pkt_type_t)), tlv_type,
                        tlv_len, tlv_value, pkt_size){

        switch(tlv_type){
            case ISIS_TLV_IF_INDEX:
                rc += sprintf(buff + rc, "%d %d %u :: ", 
                    tlv_type, tlv_len, atoi(tlv_value));
            break;
            case ISIS_TLV_HOSTNAME:
            case ISIS_TLV_RTR_ID:
            case ISIS_TLV_IF_IP:
                rc += sprintf(buff + rc, "%d %d %s :: ", tlv_type, tlv_len, tlv_value);
                break;
            case ISIS_TLV_HOLD_TIME:
                rc += sprintf(buff + rc, "%d %d %u :: ", tlv_type, tlv_len, *(uint32_t *)tlv_value);
                break;
            case ISIS_TLV_METRIC_VAL:
                rc += sprintf(buff + rc, "%d %d %u :: ", tlv_type, tlv_len, *(uint32_t *)tlv_value);
                break;
            default:    ;
        }

    } ITERATE_TLV_END(((char *)hpkt + sizeof(uint16_t)), tlv_type,
                        tlv_len, tlv_value, pkt_size)
    
    rc -= strlen(" :: ");
    pkt_info->bytes_written = rc;
}


void
isis_print_pkt(void *arg, size_t arg_size) {

    pkt_info_t *pkt_info = (pkt_info_t *)arg;

	byte *buff = pkt_info->pkt_print_buffer;
	size_t pkt_size = pkt_info->pkt_size;

    byte* pkt = (char *)(pkt_info->pkt);

	assert(pkt_info->protocol_no == ISIS_ETH_PKT_TYPE);

    isis_pkt_type_t pkt_type = ISIS_PKT_TYPE(pkt);

    switch(pkt_type) {
        case ISIS_PTP_HELLO_PKT_TYPE:
            isis_print_hello_pkt(pkt_info);
            break;
        case ISIS_LSP_PKT_TYPE:
            isis_print_lsp_pkt(pkt_info);
            break;
        default: ;
    }
}

void
isis_cancel_lsp_pkt_generation_task(node_t *node) {

    isis_node_info_t *isis_node_info = ISIS_NODE_INFO(node);
    
    if (!isis_node_info ||
         !isis_node_info->lsp_pkt_gen_task) {

        return;
    }

    task_cancel_job(isis_node_info->lsp_pkt_gen_task);
    isis_node_info->lsp_pkt_gen_task = NULL;    
}

uint32_t *
isis_get_lsp_pkt_rtr_id(isis_pkt_t *lsp_pkt) {

    ethernet_hdr_t *eth_hdr = (ethernet_hdr_t *)lsp_pkt->pkt;
    isis_pkt_hdr_t *lsp_hdr = (isis_pkt_hdr_t *)(eth_hdr->payload);

   return &lsp_hdr->rtr_id;
}

uint32_t *
isis_get_lsp_pkt_seq_no(isis_pkt_t *lsp_pkt) {

    ethernet_hdr_t *eth_hdr = (ethernet_hdr_t *)lsp_pkt->pkt;
    isis_pkt_hdr_t *lsp_hdr = (isis_pkt_hdr_t *)(eth_hdr->payload);

   return &lsp_hdr->seq_no;
}

uint32_t
isis_deref_isis_pkt(isis_pkt_t *lsp_pkt) {
    
    uint32_t rc;

    assert(lsp_pkt->pkt && 
           lsp_pkt->pkt_size &&
           lsp_pkt->ref_count);

    lsp_pkt->ref_count--;

    rc = lsp_pkt->ref_count;

    if (lsp_pkt->ref_count == 0) {

        assert(!lsp_pkt->installed_in_db);

        tcp_ip_free_pkt_buffer(lsp_pkt->pkt, lsp_pkt->pkt_size);
        
        if (lsp_pkt->expiry_timer) {

            isis_timer_data_t *timer_data = (isis_timer_data_t *)
                            wt_elem_get_and_set_app_data(lsp_pkt->expiry_timer, 0);
            free(timer_data);
            timer_de_register_app_event(lsp_pkt->expiry_timer);
            lsp_pkt->expiry_timer = NULL;
        }
        free(lsp_pkt);
    }    

    return rc;
}

void
isis_ref_isis_pkt(isis_pkt_t *isis_pkt) {

    assert(isis_pkt->pkt && 
           isis_pkt->pkt_size);

    isis_pkt->ref_count++;
}

isis_pkt_hdr_flags_t
isis_lsp_pkt_get_flags(isis_pkt_t *lsp_pkt) {

    ethernet_hdr_t *eth_hdr = (ethernet_hdr_t *)lsp_pkt->pkt;
    isis_pkt_hdr_t *lsp_hdr = (isis_pkt_hdr_t *)(eth_hdr->payload);
    return lsp_hdr->flags;
}