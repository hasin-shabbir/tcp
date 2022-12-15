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

#define DEBUG_MODE 0

#define STDIN_FD 0
#define RETRY  120 //millisecond
#define TRANSMISSION_END_INCREMENT 1
#define SIG_REPEAT 99 //signal of 3 repeated acks
#define ALPHA 0.125 //for estimatedRTT calculation
#define BETA 0.25 //for devRTT calculation
#define INIT_SSTRESH 64 //initial ssthresh
#define INIT_CWND 1.0f //initial cwnd

int next_seqno=0; //bytes-based
int next_seqno_index = 0; //corresponding discrete index
int send_base=0; //bytes-based
int send_base_index=0; //corresponding discrete index
int file_end_seqno = -1; //seq number of last packet of data
int window_size = 10;
int timer_active = 0; //bool indicator
int duplicate_ACK_count = 0; //count number of duplicate ACKs

float cwnd = INIT_CWND; //congestion window
int ssthresh = INIT_SSTRESH; //ssthresh
int slow_start = 1; //indicator of slow start phase
int congestion_avoidance = 0; //indicator of congestion avoidance phase

tcp_packet* PACKET_BUFFER[PACKET_BUFFER_SIZE]; //buffer to store unacked packets

int sockfd, serverlen;
struct sockaddr_in serveraddr;

struct itimerval timer; 
struct timeval transmission_startTime, transmission_endTime; //time when transmission started and when ACK received to calculate rto

unsigned long rto = RETRY; //rto time
double sampleRTT = 0.0; //sample rtt
double estimatedRTT = 0.0; //estimated rtt
double devRTT = 0.0; //dev rtt

tcp_packet *sndpkt;
tcp_packet *recvpkt;
sigset_t sigmask;     

#define logCWND_filename "log_cwnd.csv" //file to store cwnd change logs

void start_timer(); //method to start timer
void stop_timer(); //method to store timer
void resend_packets(int); //signal handler to resend packets
void init_timer(int delay, void (*sig_handler)(int)); //initialize timer
void reset_timer_rtt(); //reset timer on rtt recalculation
void logger_cwnd(); //method to log cwnd to file
void reset_logger_cwnd(); //method to reset cwnd logfile at start of program

int main (int argc, char **argv){
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

    //reset cwnd logfile at program start
    reset_logger_cwnd();

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


    //initialize timer
    init_timer(rto, resend_packets);
    //initialize next sequence number
    next_seqno = 0;
    //indicator for end of file
    int file_end = 0;
    while (1)
    {
        //while space for more packets and not EOF, send packets
        while (next_seqno_index<send_base_index+(int)cwnd && !file_end){
            len = fread(buffer, 1, DATA_SIZE, fp);
            if ( len <= 0){
                //if EOF, set flag and store seq num of last sent packet
                VLOG(INFO, "End Of File has been reached");
                file_end = 1;
                file_end_seqno = next_seqno;
                break;
            }
            else{
                //if not EOF
                //make packet
                sndpkt = make_packet(len);
                memcpy(sndpkt->data, buffer, len);
                //store seq no.
                sndpkt->hdr.seqno = next_seqno;

                //store packet into buffer
                if(PACKET_BUFFER[next_seqno_index%(PACKET_BUFFER_SIZE)]!=NULL){
                    free(PACKET_BUFFER[next_seqno_index%(PACKET_BUFFER_SIZE)]);
                }
                PACKET_BUFFER[next_seqno_index%(PACKET_BUFFER_SIZE)]=sndpkt;

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
                    //indicate that timer is active
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

        //slow start mechanism
        if (slow_start){
            cwnd+=1.0;
            logger_cwnd(); //log cwnd
            //change to congestion avoidance
            if ((int)cwnd==ssthresh){
                slow_start = 0;
                congestion_avoidance = 1;
            }
        }
        //congestion avoidance
        else if (congestion_avoidance){
            cwnd+=1.0/cwnd;
            logger_cwnd(); //log cwnd
        }

        //check if duplicate ACK
        if (recvpkt->hdr.ackno!=send_base){
            duplicate_ACK_count = 1;
        }
        else{
            duplicate_ACK_count += 1;
        }

        //if 3 duplicate ACKs, resend packets
        if (duplicate_ACK_count%3==0 && !file_end){
            stop_timer();
            timer_active = 0;
            
            resend_packets(SIG_REPEAT);

            continue;
        }

        //move sendbase according to the acknowledged packet
        send_base = recvpkt->hdr.ackno;
        send_base_index = ceil((float)send_base/(float)DATA_SIZE); //calculate index of sendbase

        //if last expected ACK, send EOF packet
        if (send_base==file_end_seqno){
            sndpkt = make_packet(0);
            sndpkt->hdr.seqno = next_seqno;
            if(PACKET_BUFFER[next_seqno_index%PACKET_BUFFER_SIZE]!=NULL){
                free(PACKET_BUFFER[next_seqno_index%PACKET_BUFFER_SIZE]);
            }
            PACKET_BUFFER[next_seqno_index%PACKET_BUFFER_SIZE]=sndpkt;
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

            //wait for EOF ACK
            recvpkt = (tcp_packet *)buffer;
            while (recvpkt->hdr.ackno!=next_seqno){
                if(recvfrom(sockfd, buffer, MSS_SIZE, 0, (struct sockaddr *) &serveraddr, (socklen_t *)&serverlen) < 0){
                    error("recvfrom");
                }
                recvpkt = (tcp_packet *)buffer;

            }
            break; //exit when EOF ACKed
        }
        //if send_base at next_seqno, stop timer
        if(send_base==next_seqno){
            stop_timer();
            //get time when ACK recvd
            gettimeofday(&transmission_endTime, NULL);
            
            //reset timer based on rtt estimator
            reset_timer_rtt();

            //stop timer because no inflight packets
            timer_active=0;
        }
        //restart timer again if an ACK recvd
        else{
            //get time when ACK recvd
            gettimeofday(&transmission_endTime, NULL);

            //reset timer based on rtt estimator
            reset_timer_rtt();

            //restart timer because ACK received
            start_timer();
            //get time for next inflight packet
            gettimeofday(&transmission_startTime, NULL);
            
            timer_active=1;
        }

    }

    return 0;

}



//timer start method
void start_timer(){
    sigprocmask(SIG_UNBLOCK, &sigmask, NULL);
    setitimer(ITIMER_REAL, &timer, NULL);
}

//timer stop method
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

//method to resent packets based on a singal
void resend_packets(int sig){
    //if timeout signal
    if (sig == SIGALRM)
    {
        //Resend cwnd number of packets range between 
        //sendBase and nextSeqNum
        VLOG(INFO, "Timeout happened");

        int count = 0; //count of number of packets sent
        int curr = send_base_index; //start resending from send base

        //recalculate ssthresh, cwnd
        //change to slow start mode
        ssthresh = MAX(cwnd/2,2);
        congestion_avoidance = 0;
        slow_start = 1;
        cwnd = INIT_CWND;
        //log cwnd
        logger_cwnd();
        
        if (DEBUG_MODE){
            printf("\ntimeout window: %d\n",(int)cwnd);
        }

        //resend packets between sendbase and next seq number
        //only send floor(cwnd) num of packets
        while(curr<next_seqno_index && count<(int)cwnd){
            
            if (DEBUG_MODE){
                printf("timeout resend: %d\n",curr);
            }

            if(sendto(sockfd, PACKET_BUFFER[curr%PACKET_BUFFER_SIZE], TCP_HDR_SIZE + get_data_size(PACKET_BUFFER[curr%PACKET_BUFFER_SIZE]), 0, 
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
            count+=1;
        }
        if (DEBUG_MODE){
            printf("\n");
        }

    }

    else if (sig == SIG_REPEAT)
    {
        //Resend cwnd number of packets range between 
        //sendBase and nextSeqNum
        VLOG(INFO, "3 Duplicate ACKs received");
        
        int count = 0; //count of number of packets sent
        int curr = send_base_index; //start resending from send base

        //recalculate ssthresh, cwnd
        //change to slow start mode
        ssthresh = MAX(cwnd/2,2);
        congestion_avoidance = 0;
        slow_start = 1;
        cwnd = INIT_CWND;
        //log cwnd
        logger_cwnd();

        if (DEBUG_MODE){
            printf("\nresend window: %d\n",(int)cwnd);
        }

        //resend packets between sendbase and next seq number
        //only send floor(cwnd) num of packets
        while(curr<next_seqno_index && count<(int)cwnd){
            if (DEBUG_MODE){
                printf("resending: %d\n",curr);
            }

            if(sendto(sockfd, PACKET_BUFFER[curr%PACKET_BUFFER_SIZE], TCP_HDR_SIZE + get_data_size(PACKET_BUFFER[curr%PACKET_BUFFER_SIZE]), 0, 
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
            count+=1;
        }
        if (DEBUG_MODE){
            printf("\n");
        }

    }
}

//recalculate rto based on rtt estimation and reset timer
void reset_timer_rtt(){
    sampleRTT = ((double) transmission_endTime.tv_sec - (double) transmission_startTime.tv_sec)*1000 + ((double) transmission_endTime.tv_usec - (double) transmission_startTime.tv_usec)/1000;
    estimatedRTT = MAX(((1.0 - (double) ALPHA) * estimatedRTT + (double) ALPHA * sampleRTT), 1.0);
    devRTT = MAX(((1.0 - (double) BETA) * devRTT + (double) BETA * abs(estimatedRTT - sampleRTT)), 1.0);
    rto = MAX(floor(estimatedRTT + 4 * devRTT), 1);

    init_timer(rto, resend_packets);
}

//open log file and append cwnd to it
void logger_cwnd(){
    struct timeval presentTime;
    gettimeofday(&presentTime, NULL);
    FILE *log_file = fopen(logCWND_filename,"a");
    if (log_file==NULL){
        error("failed to open log file\n");
    }
    fprintf(log_file, "%.6f,%.2f\n", (double)presentTime.tv_sec+(double)presentTime.tv_usec/1000000, cwnd);

    fclose(log_file);
}

//reset the cwnd log file at start of program
void reset_logger_cwnd(){
    FILE *log_file = fopen(logCWND_filename,"w");
    fclose(log_file);
}