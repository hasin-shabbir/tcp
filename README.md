# Simple TCP Tahoe application

## Description
This project demonstrates a simple implementation of TCP Tahoe, consisting
of two separate programs: an sender and a receiver. The sender is responsible for
transferring the file using TCP, sliding window, and congestion control mechanisms. The receiver acknowledges in-order packets received, buffers out
of order packets, sends acknowledgement for the last in-order packet received upon received an out of order packet, and sends cumulative acknowdlegement
for when a sequence of packets becomes in-order. The sender retransmits packets after a timeout (using rtt estimation) and upon 3 duplicate acknowledgements
 (fast-retransmit). The implementation of the congestion control will consists of the following features:
 <ul>
  <li>Slow-start</li>
  <li>Congestion avoidance</li>
  <li>Fast retransmit (no fast-recovery)</li>
 </ul>
 
 Additionally, the sender logs the changes in the congestion window (cwnd) along with the timestamp. Plot.py contains the script to graph cwnd against time.
## How to

1. Make file by running ```make```
2. (From the obj directory): ./rdt_receiver <port> <RECEIVE_FILE>
3. (From the obj directory): ./rdt_sender <hostname> <port> <FILE>

### To emulate it under different network conditions:
1. Install [mahimahi](http://mahimahi.mit.edu/)
2. mm-delay 10 mm-loss uplink 0.1 mm-link --meter-all channel_traces/rapidGold channel_traces/rapidGold
 
NOTE: The above command spawns a shell with a delayed, lossy link (with delay of 10 ms and uplink loss of 0.1). Read mahimahi documentation for different options 
  available. The values can be altered. mm-link --meter-all uses trace files to emulate a variable cellular network and visualize the process's use of the network.
  Different trace files have been provided but you may use your own or those that come with mahimahi. The visualizations include uplink and downlink queueing 
  delays and throughputs.
  
### Plotting cwnd
  After a successful run of the application, use the following from the tcp directory:
  1. python3 plot.py -p log_cwnd.csv -n OUTPUT_FILE
  
  NOTE: -p flag for path to cwnd logfile, -n flag for name of output file
  
