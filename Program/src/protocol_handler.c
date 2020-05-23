/*------------------------------------------------------------------------------
This file is protected under copyright. If you want to use it,
please include this text, that is my only stipulation.  

Author: Evan Giese
------------------------------------------------------------------------------*/
#include "mib.h"
#include "port.h"
#include "protocol_handler.h"
#include "packet.h"
#include "filesystem_funcs.h"
#include "requests.h"
#include "types.h"
#include "utils.h"


static void build_temperary_file(Request *req, uint32_t size) {

    ssp_snprintf(req->source_file_name, 75, "%s%llu%s", "incomplete_requests/.temp_", req->transaction_sequence_number, ".jpeg");
    ssp_printf("haven't received metadata yet, building temperary file %s\n", req->source_file_name);
    req->file = create_temp_file(req->source_file_name, size);
}


static void send_ack(Request *req, Response res, unsigned int type){
    if (req->transmission_mode == UN_ACKNOWLEDGED_MODE)
        return;

    uint8_t start = build_pdu_header(req->buff, req->transaction_sequence_number, 1, &req->pdu_header);
    build_ack(req->buff, start, type);
    ssp_sendto(res);
}

static void send_nak(Request *req, Response res, unsigned int type) {
    if (req->transmission_mode == UN_ACKNOWLEDGED_MODE)
        return;
    
    uint8_t start = build_pdu_header(req->buff, req->transaction_sequence_number, 1, &req->pdu_header);
    build_nak_directive(req->buff, start, type);
    ssp_sendto(res);
}

static void resend_eof_ack(Request *req, Response res) {

    ssp_printf("sending eof ack transaction: %d\n", req->transaction_sequence_number);
    send_ack(req,res, EOF_PDU);
    req->resent_eof++;
}

/*------------------------------------------------------------------------------

                                    Processing Packets

------------------------------------------------------------------------------*/

//for finding the struct in the list
struct request_search_params {
    uint32_t source_id;
    uint32_t transaction_sequence_number;
};

//for finding the struct in the list
static int find_request(void *element, void *args) {
    Request *req = (Request *) element;
    struct request_search_params *params = (struct request_search_params *) args;
    if (req->dest_cfdp_id == params->source_id && req->transaction_sequence_number == params->transaction_sequence_number){
      return 1;
    }
    return 0;
}


 Request *new_incomming_request(uint32_t source_id, 
        uint32_t transmission_mode, 
        uint32_t transaction_sequence_number,
        Response res,
        FTP *app) {

        Remote_entity remote_entity;
        int error = get_remote_entity_from_json(&remote_entity, source_id);
        if (error < 0) {
            ssp_error("could not get remote entity for incoming packet \n");
            return NULL;
        }
        Pdu_header pdu_header;
        error = get_header_from_mib(&pdu_header, remote_entity, app->my_cfdp_id);
        if (error < 0) {
            ssp_printf("Couldn't make PDU HEADER IS NULL\n");
            return NULL;
        }

        Request *found_req = init_request(app->buff, app->packet_len);
        if (found_req == NULL) {
            ssp_error("could not get allocate for new request \n");
            return NULL;
        }
        
        ssp_printf("incoming new request\n");

        //Make new request and add it
        found_req->transmission_mode = transmission_mode;
        found_req->transaction_sequence_number = transaction_sequence_number;
        found_req->dest_cfdp_id = source_id;
        found_req->pdu_header = pdu_header;

        found_req->remote_entity = remote_entity;
        found_req->procedure = sending_put_metadata;

        found_req->res.addr = ssp_alloc(1, res.size_of_addr);
        
        if (found_req->res.addr == NULL) {
            ssp_cleanup_req(found_req);
            return NULL;
        }

        memcpy(found_req->res.addr, res.addr, res.size_of_addr);

        found_req->res.packet_len = remote_entity.mtu;
        found_req->res.sfd = res.sfd;
        found_req->res.transmission_mode = app->remote_entity.default_transmission_mode;
        found_req->res.type_of_network = app->remote_entity.type_of_network;
        found_req->res.msg = found_req->buff;

        found_req->paused = false;
        return found_req;
}


/*creates a request struct if there is none for the incomming request based on transaction sequence number or
finds the correct request struct and replaces req with the new pointer. Returns the possition in the packet 
where the data portion is, returns -1 on fail*/
int process_pdu_header(char*packet, uint8_t is_server, Response res, Request **req, List *request_list, FTP *app) {

    uint8_t packet_index = PACKET_STATIC_HEADER_LEN;
    Pdu_header *header = (Pdu_header *) packet;

    uint32_t source_id = 0;
    memcpy(&source_id, &packet[packet_index], header->length_of_entity_IDs);
    packet_index += header->length_of_entity_IDs;

    //TODO the transaction number should get the request from data structure hosting requests
    uint32_t transaction_sequence_number = 0;
    memcpy(&transaction_sequence_number, &packet[packet_index], header->transaction_seq_num_len);
    packet_index += header->transaction_seq_num_len;

    uint32_t dest_id = 0;
    memcpy(&dest_id, &packet[packet_index], header->length_of_entity_IDs);
    packet_index += header->length_of_entity_IDs;

    if (app->my_cfdp_id != dest_id){
        ssp_printf("someone is sending packets here that are not for my id %u, dest_id: %u\n", app->my_cfdp_id, dest_id);
        return -1;
    }

    //if packet is from the same request, don't' change current request
    Request *current_req = (*req);

    uint16_t len = get_data_length(packet);
    if (len > app->packet_len){
        ssp_printf("packet received %d that was too big for our buffer %d\n", len, app->packet_len);
        return -1;
    }

    if (current_req != NULL && current_req->transaction_sequence_number == transaction_sequence_number && current_req->dest_cfdp_id == source_id){        
        return packet_index;
    }

    //look for active request in list
    struct request_search_params params = {
        source_id,
        transaction_sequence_number,
    };

    Request *found_req = (Request *) request_list->find(request_list, 0, find_request, &params);

    //if server, create new request
    if (found_req == NULL && is_server) 
    {
   
        found_req = new_incomming_request(source_id, 
            header->transmission_mode, 
            transaction_sequence_number,
            res,
            app);

        if (found_req == NULL) {
            ssp_error("could not create request for incomming transmission");
            return -1;
        }

        app->request_list->push(app->request_list, found_req, transaction_sequence_number);
    } 

    else if (found_req == NULL) {
        ssp_error("could not find request \n");
        return -1;
    }

    *req = found_req;
    return packet_index;

}

static void write_packet_data_to_file(char *data_packet, uint32_t data_len, File *file) {


    if(file == NULL) {
        ssp_error("file struct is null, can't write to file");
        return;
    }

    File_data_pdu_contents *packet = (File_data_pdu_contents *)data_packet;
    
    uint32_t offset_start = packet->offset;
    uint32_t offset_end = offset_start + data_len - 4;
    
    if (!receive_offset(file, 0, offset_start, offset_end))
        return;

    int bytes = write_offset(file, &data_packet[4], data_len - 4, offset_start);
    if (bytes <= 0) {
        ssp_error("no new data was written\n");
        return;
    }

    file->partial_checksum += calc_check_sum(&data_packet[4], bytes);
    
    if (file->missing_offsets->count == 0)
        return;

}


uint32_t fill_request_pdu_metadata(char *meta_data_packet, Request *req_to_fill) {

    Pdu_meta_data *meta_data = (Pdu_meta_data *) meta_data_packet;
    req_to_fill->segmentation_control = meta_data->segmentation_control;

    uint8_t packet_index = 1;
    uint32_t *network_bytes = (uint32_t*) &meta_data_packet[packet_index];
    uint32_t file_size = ssp_ntohl(*network_bytes);

    req_to_fill->file_size = file_size;
    packet_index += 4;

    uint8_t file_name_len = meta_data_packet[packet_index];
    packet_index++;

    memcpy(req_to_fill->source_file_name, &meta_data_packet[packet_index], file_name_len);
    packet_index += file_name_len;

    file_name_len = meta_data_packet[packet_index];
    packet_index++;
    memcpy(req_to_fill->destination_file_name, &meta_data_packet[packet_index], file_name_len);

    packet_index += file_name_len;

    return packet_index;
}


void process_messages(Request *req, FTP *app) {

    if (req->messages_to_user->count == 0)
        return;
        
    Message *message = req->messages_to_user->pop(req->messages_to_user);
    if (message->header.message_type == PROXY_PUT_REQUEST){
        
        Message_put_proxy *p = (Message_put_proxy *) message->value;
        ssp_printf("received proxy request for source file name: %s dest file name %s, to id %d\n", 
        (char *)p->source_file_name.value,
        (char *)p->destination_file_name.value,
        *(uint8_t*)p->destination_id.value);

        start_request(put_request(*(uint8_t*)p->destination_id.value,
        (char *)p->source_file_name.value, 
        (char *)p->destination_file_name.value, ACKNOWLEDGED_MODE, app));
        ssp_free_message(message);

    }   

}


/*------------------------------------------------------------------------------

                                    REMOTE SIDE
                                    aka: handles responses from server

------------------------------------------------------------------------------*/

static void resend_finished_ack(Request *req, Response res) {

    ssp_printf("sending finished packet transaction ack: %d\n", req->transaction_sequence_number);
    send_ack(req, res, FINISHED_PDU);
    req->resent_finished++;
}

static void send_put_metadata(Request *req, Response res) {

    uint32_t start = build_pdu_header(req->buff, req->transaction_sequence_number, req->transmission_mode, &req->pdu_header);
    ssp_printf("sending metadata transaction: %d\n", req->transaction_sequence_number);
    start = build_put_packet_metadata(req->buff, start, req);
    req->local_entity.Metadata_sent_indication = true;
    ssp_sendto(res);
}

static void send_eof_pdu(Request *req, Response res) {
    uint32_t start = build_pdu_header(req->buff, req->transaction_sequence_number, req->transmission_mode, &req->pdu_header);
    ssp_printf("sending eof transaction: %d\n", req->transaction_sequence_number);
    if (req->file_size == 0)
        build_eof_packet(req->buff, start, 0, 0);
    else 
        build_eof_packet(req->buff, start, req->file->total_size, req->file->partial_checksum);
    
    req->local_entity.EOF_sent_indication = true;
    ssp_sendto(res);
    return;
}

static void start_sequence(Request *req, Response res) {
    
    send_put_metadata(req, res);
    if (req->file_size == 0 ){
        req->procedure = none;
        return;
    }
    req->procedure = sending_data;
}

static void send_data(Request *req, Response res) {    
    uint32_t start = build_pdu_header(req->buff, req->transaction_sequence_number, req->transmission_mode, &req->pdu_header);

    if (build_data_packet(req->buff, start, req->file, res.packet_len)) {
        req->procedure = sending_eof;
        ssp_printf("sending data blast transaction: %d\n", req->transaction_sequence_number);
    }
    ssp_sendto(res);
}



int nak_response(char *packet, uint32_t start, Request *req, Response res, Client *client) {
        uint32_t packet_index = start;
        Pdu_nak *nak = (Pdu_nak *) &packet[packet_index];
        //uint32_t offset_first = ssp_ntohl(nak->start_scope);
        //uint32_t offset_last = ssp_ntohl(nak->end_scope);
        uint64_t segments = ntohll(nak->segment_requests);
        packet_index += 16;

        if (req->buff == NULL){
            ssp_printf("req->buff is null\n");
        }

        uint32_t outgoing_packet_index = build_pdu_header(req->buff, req->transaction_sequence_number, 0, &client->pdu_header);
        uint32_t offset_start = 0;
        uint32_t offset_end = 0;
    
        for (int i = 0; i < segments; i++){
            //outgoing_packet_index
            memcpy(&offset_start, &packet[packet_index], 4);
            offset_start = ssp_ntohl(offset_start);
            packet_index += 4;
            memcpy(&offset_end, &packet[packet_index], 4);
            offset_end = ssp_ntohl(offset_end);
            packet_index += 4;
            build_nak_response(req->buff, outgoing_packet_index, offset_start, req, client);
            ssp_sendto(res);
        }
        
        return packet_index;

}


//fills the current request with packet data, responses from servers
void parse_packet_client(char *packet, uint32_t packet_index, Response res, Request *req, Client* client) {
 

    //Pdu_header *header = (Pdu_header *) packet;    
    //uint16_t incoming_packet_data_len = ntohs(header->PDU_data_field_len);
    uint8_t directive = packet[packet_index];
    packet_index += 1; 

    switch(directive) {
        case FINISHED_PDU:
            req->local_entity.transaction_finished_indication = true;
            req->procedure = sending_finished;
            ssp_printf("received finished pdu transaction: %d\n", req->transaction_sequence_number);
            break;
        case NAK_PDU:
            nak_response(packet, packet_index, req, res, client);
            ssp_printf("received Nak pdu transaction: %d\n", req->transaction_sequence_number);
            break;
        case ACK_PDU:

            switch (packet[packet_index])
            {
                case EOF_PDU:
                    ssp_printf("received Eof ack transaction: %d\n", req->transaction_sequence_number);
                    req->local_entity.EOF_recv_indication = true;
                    break;
            
                case META_DATA_PDU:
                    ssp_printf("received Eof ack transaction: %d\n", req->transaction_sequence_number);
                    req->local_entity.Metadata_recv_indication = true;

                default:
                    break;
            }

            break;
        case NAK_DIRECTIVE:
            switch (packet[packet_index])
            {
                case META_DATA_PDU:
                    ssp_printf("resending metadata transaction: %d\n", req->transaction_sequence_number);
                    req->procedure = sending_put_metadata;
                    break;
            
                case EOF_PDU: 
                    ssp_printf("resending eof transaction: %d\n", req->transaction_sequence_number);
                    req->procedure = sending_eof;
                    break;
                
                default:
                    break;
            }

            break;
        default:
            break;
    }
}
static void check_req_status(Request *req) {

    if (req->resent_finished >= RESEND_FINISHED_TIMES){
        req->procedure = none;
    } 
}

//current user request, to send to server
void user_request_handler(Response res, Request *req, Client* client) {

    if (req == NULL || req->paused)
        return;
    
    check_req_status(req);

    switch (req->procedure)
    {
        case sending_eof: 
            send_eof_pdu(req, res);
            req->procedure = none;
            break;

        case sending_data:
            send_data(req, res);
            break;

        case sending_put_metadata:
            send_put_metadata(req, res);
            req->procedure = none;
            break;

        case sending_finished:
            resend_finished_ack(req, res);
            req->procedure = none;
            break;

        case sending_start:
            start_sequence(req, res);
            break;

        case clean_up: // will close the request happens in the previous functions
        case none:
            break;
        default:
            break;
    }
}
/*------------------------------------------------------------------------------

                                    SERVER SIDE
                                    aka: handles responses from remote

------------------------------------------------------------------------------*/



/*
static void print_offsets(void *element, void *args) {

    Offset *off = (Offset *) element;
    ssp_printf("missing offset start: %d end:%d\n", off->start, off->end);
}
*/

static void request_eof(Request *req, Response res) {
    
    ssp_printf("sending eof nak transaction: %d\n", req->transaction_sequence_number);
    send_nak(req, res, EOF_PDU);
}

static void request_metadata(Request *req, Response res) {

    ssp_printf("sending request for new metadata packet %d\n", req->transaction_sequence_number);
    send_nak(req, res, META_DATA_PDU);
}

static void request_data(Request *req, Response res) {

    uint8_t start = build_pdu_header(req->buff, req->transaction_sequence_number, 1, &req->pdu_header);
    ssp_printf("sending Nak data transaction: %d\n", req->transaction_sequence_number);
    build_nak_packet(req->buff, start, req);
    ssp_sendto(res);
}


static void resend_finished_pdu(Request *req, Response res) {

    uint8_t start = build_pdu_header(req->buff, req->transaction_sequence_number, 1, &req->pdu_header);
    ssp_printf("sending finished pdu transaction: %d\n", req->transaction_sequence_number);
    build_finished_pdu(req->buff, start);
    ssp_sendto(res);
    req->resent_finished++;   
}


//processes the eof packet, sets checksum, indication, and filesize.
void process_pdu_eof(char *packet, Request *req, Response res) {


    ssp_printf("received eof packet transaction: %d\n", req->transaction_sequence_number);
    Pdu_eof *eof_packet = (Pdu_eof *) packet;
    uint32_t file_size = ssp_ntohl(eof_packet->file_size);

    if (req->file == NULL && req->local_entity.Metadata_recv_indication) {
        build_temperary_file(req, file_size);
    }

    req->local_entity.EOF_recv_indication = 1;
    req->file->eof_checksum = eof_packet->checksum;
    req->file->total_size = file_size;
    
}

int process_file_request_metadata(Request *req) {

    char temp[75];
    if (req->file == NULL)
        req->file = create_file(req->destination_file_name, 1);

    else if (req->file->is_temp) {
        ssp_snprintf(temp, 75, "%s%llu%s", "incomplete_requests/.temp_", req->transaction_sequence_number, ".jpeg");
        change_tempfile_to_actual(temp, req->destination_file_name, req->file_size, req->file);
        return 1;
    }
    
    Offset *offset = ssp_alloc(1, sizeof(Offset));
    offset->end = req->file_size;
    offset->start = 0;
    req->file->missing_offsets->insert(req->file->missing_offsets, offset, req->file_size);

    return 1;
}

static void process_metadata(char *packet, uint32_t packet_index, Response res, Request *req, FTP *app) {

    req->procedure = sending_put_metadata;
    req->local_entity.Metadata_recv_indication = true;

    ssp_printf("received metadata packet transaction: %d\n", req->transaction_sequence_number);
    packet_index += fill_request_pdu_metadata(&packet[packet_index], req);

    uint16_t data_len = get_data_length(packet);

    get_messages_from_packet(packet, packet_index, data_len, req);
    process_messages(req, app);
    
    send_ack(req, res, META_DATA_PDU);

    if (req->file_size != 0) 
        process_file_request_metadata(req);
    else {
        printf("just receiving messages, closing request\n");
        //req->local_entity.transaction_finished_indication = true;
        req->local_entity.EOF_recv_indication = true;
        req->procedure = none;
    }
}


void on_server_time_out(Response res, Request *req) {
    

    if (req->paused)
        return;

    if (req->transmission_mode == UN_ACKNOWLEDGED_MODE ||
        req->local_entity.transaction_finished_indication)
        return;


    if (req->resent_finished == RESEND_FINISHED_TIMES && req->local_entity.transaction_finished_indication) {
        req->procedure = clean_up;
        ssp_printf("file sent, closing request transaction: %d\n", req->transaction_sequence_number);
        return;
    }
    //send request for metadata
    if (!req->local_entity.Metadata_recv_indication) {
        request_metadata(req, res);
        return;
    }

    //receiving just messages, send back finished
    if (req->file_size == 0 && RESEND_FINISHED_TIMES != req->resent_finished) {
        resend_finished_pdu(req, res);
        return;
    }

    //send missing eofs
    if (!req->local_entity.EOF_recv_indication) {
        request_eof(req, res);
    }

    //received EOF, send back 3 eof ack packets
    else if (req->local_entity.EOF_recv_indication && req->resent_eof < RESEND_EOF_TIMES) {
        resend_eof_ack(req, res);
    }

    
    //if have not received metadata for a file tranaction, this should not ever trigger //TODO add asert
    if (req->file == NULL) {
        ssp_printf("file is null, not sending data naks");
        return;
    }
    //send missing NAKS
    if (req->file->missing_offsets->count > 0) {
        request_data(req, res);
        return;

    } else {
        //finished transaction, should have checksum complete, and received eof notification
        if (req->file->eof_checksum == req->file->partial_checksum && req->local_entity.EOF_recv_indication) {
            req->local_entity.transaction_finished_indication = true;
            resend_finished_pdu(req, res);
            return;
        }
        
        ssp_printf("checksum have: %u checksum_need: %u\n", req->file->partial_checksum, req->file->eof_checksum);
        
    }

}

//fills the current_request struct for the server, incomming requests
int parse_packet_server(char *packet, uint32_t packet_index, Response res, Request *req, FTP *app) {

    if (packet_index == 0)
        return -1;
        
    Pdu_header *header = (Pdu_header *) packet;
    uint16_t data_len = get_data_length(packet);
    uint32_t packet_len = packet_index + data_len;

    //process file data
    if (header->PDU_type == 1) {
        if (!req->local_entity.Metadata_recv_indication) {
            if (req->file == NULL) {
                printf("file is null\n");
                build_temperary_file(req, TEMP_FILESIZE);
            }
        }
        write_packet_data_to_file(&packet[packet_index], data_len, req->file);
        ssp_printf("received data packet transaction: %d\n", req->transaction_sequence_number);
        return packet_len;
    }
    

    Pdu_directive *directive = (Pdu_directive *) &packet[packet_index];
    packet_index++;

    switch (directive->directive_code)
    {
        case META_DATA_PDU:
            if (req->local_entity.Metadata_recv_indication)
                break;
            process_metadata(packet, packet_index, res, req, app);
            break;
    
        case EOF_PDU:
            if (req->local_entity.EOF_recv_indication)
                break;
            process_pdu_eof(&packet[packet_index], req, res);
            break;

        case ACK_PDU: 
            ssp_printf("received Ack transaction: %d\n", req->transaction_sequence_number);
            Pdu_ack* ack_packet = (Pdu_ack *) &packet[packet_index];
            switch (ack_packet->directive_code)
            {
                case FINISHED_PDU:

                    ssp_printf("received finished packet transaction: %d\n", req->transaction_sequence_number);
                    req->local_entity.transaction_finished_indication = true;
                    break;
            
                default:
                    break;
            }
            break;
        default:
            break;
    }

    return packet_len;
}

