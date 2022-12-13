#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h> 
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#include <sys/time.h>
#include <time.h>
#include <assert.h>
#include <math.h>

#include"packet.h"
#include"common.h"

#define MAX(a,b) (((a)>(b))?(a):(b));

#define STDIN_FD    0
#define RETRY  120 //millisecond
#define PACKET_BUFFER_SIZE 10
#define TRANSMISSION_END_INCREMENT 1
#define SIG_REPEAT 99
#define ALPHA 0.125 //for estimatedRTT calculation
#define BETA 0.25 //for devRTT calculation
//RTO bounds
#define MAX_RTO 2500  
#define MIN_RTO 100

int next_seqno=0; //bytes-based
int next_seqno_index = 0; //corresponding discrete index
int send_base=0; //bytes-based
int send_base_index=0; //corresponding discrete index
int file_end_seqno = -1; //seq number of last packet of data
int window_size = 10;
int timer_active = 0; //bool indicator
int duplicate_ACK_count = 0;


tcp_packet* PACKET_BUFFER[PACKET_BUFFER_SIZE];

int sockfd, serverlen;
struct sockaddr_in serveraddr;

struct itimerval timer; 
struct timeval transmission_startTime, transmission_endTime;

unsigned long rto = RETRY;
double sampleRTT = 0.0;
double estimatedRTT = 0.0;
double devRTT = 0.0;

tcp_packet *sndpkt;
tcp_packet *recvpkt;
sigset_t sigmask;       


void start_timer();
void stop_timer();
void resend_packets(int);
void init_timer(int delay, void (*sig_handler)(int));
void reset_timer_rtt();

int main (int argc, char **argv)
{
    int portno, len;
    int next_seqno;
    char *hostname;
    char buffer[DATA_SIZE];
    FILE *fp;

    /* check command line arguments */
    if (argc != 4) {
        fprintf(stderr,"usage: %s <hostname> <port> <FILE>\n", argv[0]);
        exit(0);
    }
    hostname = argv[1];
    portno = atoi(argv[2]);
    fp = fopen(argv[3], "r");
    if (fp == NULL) {
        error(argv[3]);
    }

    /* socket: create the socket */
    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) 
        error("ERROR opening socket");


    /* initialize server server details */
    bzero((char *) &serveraddr, sizeof(serveraddr));
    serverlen = sizeof(serveraddr);

    /* covert host into network byte order */
    if (inet_aton(hostname, &serveraddr.sin_addr) == 0) {
        fprintf(stderr,"ERROR, invalid host %s\n", hostname);
        exit(0);
    }

    /* build the server's Internet address */
    serveraddr.sin_family = AF_INET;
    serveraddr.sin_port = htons(portno);

    assert(MSS_SIZE - TCP_HDR_SIZE > 0);

    //Stop and wait protocol

    init_timer(rto, resend_packets);
    next_seqno = 0;
    int file_end = 0;
    while (1)
    {
        //while space for more packets and not EOF, send packets
        while (next_seqno_index<send_base_index+window_size && !file_end){
            len = fread(buffer, 1, DATA_SIZE, fp);
            if ( len <= 0){
                //if EOF, set flag and store seq num of last sent packet
                VLOG(INFO, "End Of File has been reached");
                file_end = 1;
                file_end_seqno = next_seqno;
                break;
            }
            else{
                //make packet
                sndpkt = make_packet(len);
                memcpy(sndpkt->data, buffer, len);
                //store seq no.
                sndpkt->hdr.seqno = next_seqno;

                //store packet into buffer
                if(PACKET_BUFFER[next_seqno_index%window_size]!=NULL){
                    free(PACKET_BUFFER[next_seqno_index%window_size]);
                }
                PACKET_BUFFER[next_seqno_index%window_size]=sndpkt;

                //send packet
                if(sendto(sockfd, sndpkt, TCP_HDR_SIZE + get_data_size(sndpkt), 0, 
                            ( const struct sockaddr *)&serveraddr, serverlen) < 0){
                    error("sendto");
                }
                
                //start timer if not active (oldest unacked packet in flight)
                if(!timer_active){
                    start_timer();
                    //get time when starting transmission
                    gettimeofday(&transmission_startTime, NULL);
                    timer_active=1;
                }
                // increment seq no.
                next_seqno = next_seqno + len;
                next_seqno_index +=1;
            }
        }

        //recv ACK
        if(recvfrom(sockfd, buffer, MSS_SIZE, 0, (struct sockaddr *) &serveraddr, (socklen_t *)&serverlen) < 0){
                error("recvfrom");
        }
        recvpkt = (tcp_packet *)buffer;
        //get acknowledged pack base and index
        if (recvpkt->hdr.ackno==send_base){
            duplicate_ACK_count += 1;
        }
        else{
            duplicate_ACK_count = 0;
        }
        if (duplicate_ACK_count==2){
            duplicate_ACK_count = 0;
            resend_packets(SIG_REPEAT);
        }

        send_base = recvpkt->hdr.ackno;
        send_base_index = ceil((float)send_base/(float)DATA_SIZE); //nti
        //if last expected ACK, send EOF packet
        if (send_base==file_end_seqno){
            sndpkt = make_packet(0);
            sndpkt->hdr.seqno = next_seqno;
            if(PACKET_BUFFER[next_seqno_index%window_size]!=NULL){
                free(PACKET_BUFFER[next_seqno_index%window_size]);
            }
            PACKET_BUFFER[next_seqno_index%window_size]=sndpkt;
            if(sendto(sockfd, sndpkt, TCP_HDR_SIZE + get_data_size(sndpkt), 0, 
                            ( const struct sockaddr *)&serveraddr, serverlen) < 0){
                error("sendto");
            }
            next_seqno = next_seqno + TRANSMISSION_END_INCREMENT;
            next_seqno_index +=1;
            //wait for end of file signal ACK, else need to retransmit
            if(recvfrom(sockfd, buffer, MSS_SIZE, 0, (struct sockaddr *) &serveraddr, (socklen_t *)&serverlen) < 0){
                error("recvfrom");
            }
            recvpkt = (tcp_packet *)buffer;
            if (recvpkt->hdr.ackno==next_seqno){
                break;
            }
        }
        //if send_base at next_seqno, stop timer
        if(send_base==next_seqno){
            stop_timer();
            //get time when ACK recvd
            gettimeofday(&transmission_endTime, NULL);
            
            //reset timer based on rtt estimator
            reset_timer_rtt();

            timer_active=0;
        }
        //restart timer again if an ACK recvd
        else{
            //get time when ACK recvd
            gettimeofday(&transmission_endTime, NULL);

            //reset timer based on rtt estimator
            reset_timer_rtt();

            start_timer();
            //get time for next inflight packet
            gettimeofday(&transmission_startTime, NULL);

            timer_active=1;
        }

    }

    return 0;

}




void start_timer(){
    sigprocmask(SIG_UNBLOCK, &sigmask, NULL);
    setitimer(ITIMER_REAL, &timer, NULL);
}


void stop_timer(){
    sigprocmask(SIG_BLOCK, &sigmask, NULL);
}

/*
 * init_timer: Initialize timer
 * delay: delay in milliseconds
 * sig_handler: signal handler function for re-sending unACKed packets
 */
void init_timer(int delay, void (*sig_handler)(int)){
    signal(SIGALRM, resend_packets);
    timer.it_interval.tv_sec = delay / 1000;    // sets an interval of the timer
    timer.it_interval.tv_usec = (delay % 1000) * 1000;  
    timer.it_value.tv_sec = delay / 1000;       // sets an initial value
    timer.it_value.tv_usec = (delay % 1000) * 1000;

    sigemptyset(&sigmask);
    sigaddset(&sigmask, SIGALRM);
}

void resend_packets(int sig){
    if (sig == SIGALRM)
    {
        //Resend all packets range between 
        //sendBase and nextSeqNum
        VLOG(INFO, "Timeout happened");
        // rto = rto * 2;
        // rto = -1 * MAX(-1*MAX_RTO, -1 * rto);
        // rto = MAX(rto, MIN_RTO);

        int curr = send_base_index;
        while(curr<next_seqno_index){
            if(sendto(sockfd, PACKET_BUFFER[curr%window_size], TCP_HDR_SIZE + get_data_size(PACKET_BUFFER[curr%window_size]), 0, 
                ( const struct sockaddr *)&serveraddr, serverlen) < 0){
                error("sendto");
            }
            //start timer if not already active
            if (!timer_active){
                init_timer(rto, resend_packets);
                start_timer();
                //get time when starting transmission
                gettimeofday(&transmission_startTime, NULL);
                timer_active=1;
            }
            curr+=1;
        }
        
    }

    if (sig == SIG_REPEAT)
    {
        //Resend all packets range between 
        //sendBase and nextSeqNum
        VLOG(INFO, "3 Duplicate ACKs received");
        // rto = rto * 2;
        // rto = -1 * MAX(-1*MAX_RTO, -1 * rto);
        // rto = MAX(rto, MIN_RTO);

        int curr = send_base_index;
        while(curr<next_seqno_index){
            if(sendto(sockfd, PACKET_BUFFER[curr%window_size], TCP_HDR_SIZE + get_data_size(PACKET_BUFFER[curr%window_size]), 0, 
                ( const struct sockaddr *)&serveraddr, serverlen) < 0){
                error("sendto");
            }
            //start timer if not already active
            if (!timer_active){
                init_timer(rto, resend_packets);
                start_timer();
                //get time when starting transmission
                gettimeofday(&transmission_startTime, NULL);
                timer_active=1;
            }
            curr+=1;
        }
        
    }
}

void reset_timer_rtt(){
    sampleRTT = ((double) transmission_endTime.tv_sec - (double) transmission_startTime.tv_sec)*1000 + ((double) transmission_endTime.tv_usec - (double) transmission_startTime.tv_usec)/1000;
    estimatedRTT = MAX(((1.0 - (double) ALPHA) * estimatedRTT + (double) ALPHA * sampleRTT), 1.0);
    devRTT = MAX(((1.0 - (double) BETA) * devRTT + (double) BETA * abs(estimatedRTT - sampleRTT)), 1.0);
    rto = MAX(floor(estimatedRTT + 4 * devRTT), 1);
    
    rto = -1 * MAX(-1*MAX_RTO, -1 * rto);
    rto = MAX(rto, MIN_RTO);

    init_timer(rto, resend_packets);
}