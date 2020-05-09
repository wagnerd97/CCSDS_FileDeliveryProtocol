
#include "types.h"
#include "filesystem_funcs.h"
#include "port.h"
#include "protocol_handler.h"
#include "mib.h"
#include "file_delivery_app.h"
#include "packet.h"
#include "unit_tests.h"
#include "requests.h"

#include <stdio.h>
#include <arpa/inet.h>
#include <stdlib.h>


void mock_mock_remote_entity(Remote_entity *remote_entity, uint32_t cfdp_id) {

    remote_entity->UT_address = 1;
    remote_entity->UT_port = 1;
    remote_entity->type_of_network = 1;
    remote_entity->one_way_light_time = 1;
    remote_entity->total_round_trip_allowance = 1;
    remote_entity->async_NAK_interval = 1;
    remote_entity->async_keep_alive_interval = 1;
    remote_entity->async_report_interval= 1;
    remote_entity->immediate_nak_mode_enabled= 1;
    remote_entity->prompt_transmission_interval= 1;
    remote_entity->default_transmission_mode= 1;
    remote_entity->disposition_of_incomplete= 1;
    remote_entity->CRC_required= 1;
    remote_entity->mtu= 1500;
    remote_entity->keep_alive_discrepancy_limit= 1;
    remote_entity->positive_ack_timer_expiration_limit= 1;
    remote_entity->nak_timer_expiration_limit= 1;
    remote_entity->transaction_inactivity_limit= 1;
    remote_entity->cfdp_id = cfdp_id;

}

int mock_packet(char *packet, uint32_t dest_id, uint32_t src_id) {

    Pdu_header pdu_header;
    Remote_entity remote_entity;

    mock_mock_remote_entity(&remote_entity, src_id);    

    int error = get_header_from_mib(&pdu_header, remote_entity, dest_id);
    int packet_index = build_pdu_header(packet, 1, 0, &pdu_header);

    return packet_index;
}

File *mock_eof_packet(char *packet, uint32_t dest_id, uint32_t src_id, char *file_name) {
    
    int packet_index = mock_packet(packet, dest_id, src_id);
    File *file = create_file(file_name, false);
    build_eof_packet(packet, packet_index, file->total_size, check_sum_file(file, 1000));

    return file;
}


Response *mock_response() {
    Response *res = calloc(1, sizeof(Response));
    int addr = 16;
    res->addr = &addr;
    res->sfd = 1;
    res->packet_len = 1500;
    res->size_of_addr = 16;
    res->type_of_network = posix;
    res->transmission_mode = UN_ACKNOWLEDGED_MODE;
    return res;
}


Request *mock_request() {
    Request *req = init_request(5000);

    char *dest = "dest";
    char *src = "src";
    uint32_t id = 1;

    req->dest_cfdp_id = id;
    req->file = create_file("dest_received.jpg", true);
    memcpy (req->source_file_name, dest, strnlen(dest, 255)); 
    memcpy (req->destination_file_name, src, strnlen(src, 255));

    
    mock_mock_remote_entity(&req->remote_entity, id);
    get_header_from_mib(&req->pdu_header, req->remote_entity, 1);

    int addr = 16;
    req->res.addr = malloc(5);
    memcpy(req->res.addr, &addr, 4);
    req->res.sfd = 1;
    req->res.packet_len = 2000;
    req->res.size_of_addr = 16;
    req->res.type_of_network = posix;
    req->res.transmission_mode = UN_ACKNOWLEDGED_MODE;
    req->res.msg = req->buff;


    return req;
}   


/*
static int test_process_pdu_eof() {
//char *packet, Request *req, Response res

    
    //process_pdu_eof(packet, Request *req, Response res);
    return 0;
}


*/

