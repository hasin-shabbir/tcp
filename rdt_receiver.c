#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h> 
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <assert.h>
#include <math.h>

#include "common.h"
#include "packet.h"

#define PACKET_BUFFER_SIZE 10

tcp_packet *recvpkt;
tcp_packet *sndpkt;
tcp_packet* PACKET_BUFFER[PACKET_BUFFER_SIZE];

int main(int argc, char **argv) {
    int sockfd; /* socket */
    int portno; /* port to listen on */
    int clientlen; /* byte size of client's address */
    struct sockaddr_in serveraddr; /* server's addr */
    struct sockaddr_in clientaddr; /* client addr */
    int optval; /* flag value for setsockopt */
    FILE *fp;
    char buffer[MSS_SIZE];
    struct timeval tp;
    int last_ackno = 0;
    int next_seqno = 0;

    /* 
     * check command line arguments 
     */
    if (argc != 3) {
        fprintf(stderr, "usage: %s <port> FILE_RECVD\n", argv[0]);
        exit(1);
    }
    portno = atoi(argv[1]);

    fp  = fopen(argv[2], "w");
    if (fp == NULL) {
        error(argv[2]);
    }

    /* 
     * socket: create the parent socket 
     */
    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) 
        error("ERROR opening socket");

    /* setsockopt: Handy debugging trick that lets 
     * us rerun the server immediately after we kill it; 
     * otherwise we have to wait about 20 secs. 
     * Eliminates "ERROR on binding: Address already in use" error. 
     */
    optval = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, 
            (const void *)&optval , sizeof(int));

    /*
     * build the server's Internet address
     */
    bzero((char *) &serveraddr, sizeof(serveraddr));
    serveraddr.sin_family = AF_INET;
    serveraddr.sin_addr.s_addr = htonl(INADDR_ANY);
    serveraddr.sin_port = htons((unsigned short)portno);

    /* 
     * bind: associate the parent socket with a port 
     */
    if (bind(sockfd, (struct sockaddr *) &serveraddr, 
                sizeof(serveraddr)) < 0) 
        error("ERROR on binding");

    /* 
     * main loop: wait for a datagram, then echo it
     */
    VLOG(DEBUG, "epoch time, bytes received, sequence number");

    clientlen = sizeof(clientaddr);
    while (1) {
        /*
         * recvfrom: receive a UDP datagram from a client
         */
        if (recvfrom(sockfd, buffer, MSS_SIZE, 0,
                (struct sockaddr *) &clientaddr, (socklen_t *)&clientlen) < 0) {
            error("ERROR in recvfrom");
        }
        recvpkt = (tcp_packet *) buffer;
        //ensure recv data is not greater than data_size
        assert(get_data_size(recvpkt) <= DATA_SIZE);
        //exit if EOF reached
        if ( recvpkt->hdr.data_size == 0) {
            VLOG(INFO, "End Of File has been reached");
            fclose(fp);
            //send ACK for EOF
            sndpkt = make_packet(0);
            sndpkt->hdr.ackno = recvpkt->hdr.seqno + recvpkt->hdr.data_size;
            sndpkt->hdr.ctr_flags = ACK;
            if (sendto(sockfd, sndpkt, TCP_HDR_SIZE, 0, 
                    (struct sockaddr *) &clientaddr, clientlen) < 0) {
                error("ERROR in sendto");
            }
            break;
        }
        /* 
         * sendto: ACK back to the client 
         */
        gettimeofday(&tp, NULL);
        // VLOG(DEBUG, "%lu, %d, %d", tp.tv_sec, recvpkt->hdr.data_size, recvpkt->hdr.seqno);

        //if in-order packet recvd, write the packet to file and send acknowledgement
        if (recvpkt->hdr.seqno == next_seqno) {
            printf("writing at: %d\n", recvpkt->hdr.seqno );
            
            char subbuff[8];
            memcpy(subbuff,&(recvpkt->data[0]),7);
            subbuff[7]='\0';
            printf("data start: %s\n-------------------\n",subbuff);
            fseek(fp,0, SEEK_SET);    
            fseek(fp, recvpkt->hdr.seqno, SEEK_SET);
            fwrite(recvpkt->data, 1, recvpkt->hdr.data_size, fp);
            
            sndpkt = make_packet(0);
            sndpkt->hdr.ackno = recvpkt->hdr.seqno + recvpkt->hdr.data_size;
            sndpkt->hdr.ctr_flags = ACK;
            if (sendto(sockfd, sndpkt, TCP_HDR_SIZE, 0, 
                    (struct sockaddr *) &clientaddr, clientlen) < 0) {
                error("ERROR in sendto");
            }
            last_ackno = next_seqno + recvpkt->hdr.data_size;
            next_seqno += recvpkt->hdr.data_size;

            int i = ceil((float)next_seqno/(float)DATA_SIZE);
            i = i%PACKET_BUFFER_SIZE;
            if (PACKET_BUFFER[i]!=NULL){
                PACKET_BUFFER[i]->hdr.seqno = PACKET_BUFFER[i]->hdr.seqno + PACKET_BUFFER[i]->hdr.data_size;
            }

            while (PACKET_BUFFER[i]!=NULL && PACKET_BUFFER[i]->hdr.seqno==next_seqno){
                fseek(fp,0, SEEK_SET);
                fseek(fp, PACKET_BUFFER[i]->hdr.seqno, SEEK_SET);
                printf("writing lost at: %d | %d\n",i, PACKET_BUFFER[i]->hdr.seqno );

                /*DEBUG*/
                char subbuff[8];
                memcpy(subbuff,&(PACKET_BUFFER[i]->data[0]),7);
                subbuff[7]='\0';
                printf("data start: \n%s\n-------------------\n",subbuff);
                /*END DEBUG*/

                fwrite(PACKET_BUFFER[i]->data, 1, PACKET_BUFFER[i]->hdr.data_size, fp);

                next_seqno += PACKET_BUFFER[i]->hdr.data_size;
                sndpkt = make_packet(0);
                sndpkt->hdr.ackno = PACKET_BUFFER[i]->hdr.seqno + PACKET_BUFFER[i]->hdr.data_size;
                sndpkt->hdr.ctr_flags = ACK;
                
                PACKET_BUFFER[i] = NULL;
                i = ceil((float)next_seqno/(float)DATA_SIZE);
                i = i%PACKET_BUFFER_SIZE;
                if (PACKET_BUFFER[i]!=NULL){
                    PACKET_BUFFER[i]->hdr.seqno = PACKET_BUFFER[i]->hdr.seqno + PACKET_BUFFER[i]->hdr.data_size;
                }
            }
            if (next_seqno != last_ackno) {
                if (sendto(sockfd, sndpkt, TCP_HDR_SIZE, 0, 
                        (struct sockaddr *) &clientaddr, clientlen) < 0) {
                    error("ERROR in sendto");
                }
                last_ackno = sndpkt->hdr.ackno;
            }
        }
        //if out of order packet recvd, send repeat ACK of previous in-order packet recvd
        else {
            // printf("lost packet: %d\n",next_seqno);
            sndpkt = make_packet(0);
            sndpkt->hdr.ackno = last_ackno;
            sndpkt->hdr.ctr_flags = ACK;
            if (sendto(sockfd, sndpkt, TCP_HDR_SIZE, 0, 
                    (struct sockaddr *) &clientaddr, clientlen) < 0) {
                error("ERROR in sendto");
            }
            int j = ceil((float)recvpkt->hdr.seqno/(float)DATA_SIZE);
            j = j%PACKET_BUFFER_SIZE;
            if(PACKET_BUFFER[j]==NULL && recvpkt->hdr.seqno>next_seqno){
                PACKET_BUFFER[j]=recvpkt;
                PACKET_BUFFER[j]->hdr.seqno=recvpkt->hdr.seqno;
            }
        }

    }

    return 0;
}