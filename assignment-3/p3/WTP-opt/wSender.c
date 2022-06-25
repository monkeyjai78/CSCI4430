#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netdb.h>
#include <sys/time.h>
#include <fcntl.h>
#include <errno.h>

#include "../starter_files/PacketHeader.h"
#include "../starter_files/crc32.h"

#define PACKET_LENGTH 1472
#define HEADER_LENGTH 16
#define DATA_LENGTH 1456
#define RETRANSMISSION_TIME 500
#define PACKET_TYPE_START 0
#define PACKET_TYPE_END 1
#define PACKET_TYPE_DATA 2
#define PACKET_TYPE_ACK 3

struct window_data {
  char data[DATA_LENGTH];
  int length;
  int acked;
  struct timeval rt_start;
};

void convert_header_to_bytes(struct PacketHeader * header, char * header_buffer) {
  memcpy(header_buffer , (char *) &(header->type), 4);
  memcpy(header_buffer + 4 , (char *) &(header->seqNum), 4);
  memcpy(header_buffer + 8 , (char *) &(header->length), 4);
  memcpy(header_buffer + 12 , (char *) &(header->checksum), 4);
}

int get_header_info(char * buffer, struct PacketHeader * header) {
  memcpy((char *) &header->type, buffer, 4);
  memcpy((char *) &header->seqNum, buffer + 4, 4);
  memcpy((char *) &header->length, buffer + 8, 4);
  memcpy((char *) &header->checksum, buffer + 12, 4);
}

void read_file(FILE * fp, char * buffer, int length, int * actual_read_length) {
  int read_start, read_end;

  read_start = ftell(fp);
  fread(buffer, length, 1, fp);
  read_end = ftell(fp);
  
  *actual_read_length = read_end - read_start;
}


void send_start_end_message_with_ack_recv(int type, int sock, struct sockaddr_in * dest_attr, socklen_t dest_len, char * log) {
  char header_buffer[HEADER_LENGTH];

  struct PacketHeader header;
  header.type = type;
  header.seqNum = rand();
  header.length = 0;
  header.checksum = 0;

  convert_header_to_bytes(&header, header_buffer);

  printf("Sending start/end message to %s:%d, seqNum: %d\n", inet_ntoa(dest_attr->sin_addr), ntohs(dest_attr->sin_port), header.seqNum);

  if (sendto(sock, header_buffer, HEADER_LENGTH, MSG_NOSIGNAL, (struct sockaddr *) dest_attr, dest_len) == -1) {
    perror("sendto()");
    exit(1);
  } else {
    // Log for the sent packet
    FILE * log_fp = fopen(log, "a");
    fprintf(log_fp, "%u %u %u %u\n", header.type, header.seqNum, header.length, header.checksum);
    fclose(log_fp);
  }

  char recv_buffer[PACKET_LENGTH];
  int recv_len;
  struct PacketHeader recv_header;
  struct timeval rt_start, rt_end;
  gettimeofday(&rt_start, NULL);
  while (1) {
    if (recv_len = recvfrom(sock, recv_buffer, PACKET_LENGTH, MSG_NOSIGNAL, (struct sockaddr *) dest_attr, &dest_len) == -1) {
      if (errno != EAGAIN && errno != EWOULDBLOCK) {
        perror("recvfrom");
        exit(1);
      }
    } else {
      get_header_info(recv_buffer, &recv_header);
      // Log for the received ACK
      FILE * log_fp = fopen(log, "a");
      fprintf(log_fp, "%u %u %u %u\n", recv_header.type, recv_header.seqNum, recv_header.length, recv_header.checksum);
      fclose(log_fp);
      if (recv_header.type == PACKET_TYPE_ACK && recv_header.seqNum == header.seqNum) {
        printf("Received ACK from %s:%d, seqNum: %d\n", inet_ntoa(dest_attr->sin_addr), ntohs(dest_attr->sin_port), recv_header.seqNum);
        break;
      }
    }
  
    gettimeofday(&rt_end, NULL);
    double passed_time = (rt_end.tv_sec - rt_start.tv_sec) * 1000 + (rt_end.tv_usec - rt_start.tv_usec) / (double) 1000;
    // Retransmission timeout
    if (passed_time > RETRANSMISSION_TIME) {
      // If the counted time exceeds the retransmission time, resend the packet
      printf("Resend start/end message\n");
      if (sendto(sock, header_buffer, HEADER_LENGTH, MSG_NOSIGNAL, (struct sockaddr *) dest_attr, dest_len) == -1) {
        perror("sendto()");
        exit(1);
      } else {
        // Log for the sent packet
        FILE * log_fp = fopen(log, "a");
        fprintf(log_fp, "%u %u %u %u\n", header.type, header.seqNum, header.length, header.checksum);
        fclose(log_fp);
      }
      gettimeofday(&rt_start, NULL);
    }
  }
}

void send_data_message(char * data, int data_length, int seqNum, int sock, struct sockaddr_in * dest_attr, socklen_t dest_len, char * log) {
  if (data_length <= 0) return;

  char buffer[PACKET_LENGTH];
  char header_buffer[HEADER_LENGTH];

  struct PacketHeader header;
  header.type = PACKET_TYPE_DATA;
  header.seqNum = seqNum;
  header.length = data_length;
  header.checksum = crc32(data, data_length);

  convert_header_to_bytes(&header, header_buffer);
  memset(buffer, '\0', PACKET_LENGTH);
  memcpy(buffer, header_buffer, HEADER_LENGTH);
  memcpy(buffer + HEADER_LENGTH, data, data_length);

  printf("Sending a packet to %s:%d, seqNum: %d\n", inet_ntoa(dest_attr->sin_addr), ntohs(dest_attr->sin_port), seqNum);
  if (sendto(sock, buffer, data_length + HEADER_LENGTH, MSG_NOSIGNAL, (struct sockaddr *) dest_attr, dest_len) == -1) {
    perror("sendto()");
    exit(1);
  } else {
    // Log for the sent packet
    FILE * log_fp = fopen(log, "a");
    fprintf(log_fp, "%u %u %u %u\n", header.type, header.seqNum, header.length, header.checksum);
    fclose(log_fp);
  }
}

void set_window(FILE * fp, struct window_data * window, int window_size, int keep_start_index, int keep_end_index) {
  if (keep_start_index >= 0 && keep_end_index >= 0 &&
    (keep_start_index > keep_end_index || keep_start_index >= window_size || keep_end_index >= window_size)) {
    printf("Error: Invalid keep index while set window");
    exit(1);
  }
  int read_length;
  if (keep_start_index >= 0 && keep_end_index >= 0) {
    // If keep part of the original window
    for (int i = 0; i <= keep_end_index - keep_start_index; i++) {
      window[i] = window[keep_start_index + i];
    }
    for (int i = keep_end_index - keep_start_index + 1; i < window_size; i++) {
      // Clear the previous buffer
      if (window[i].length != 0) {
        memset(window[i].data, '\0', DATA_LENGTH);
        window[i].length = 0;
      }
      if (!feof(fp)) {
        window[i].acked=0;
        read_file(fp, window[i].data, DATA_LENGTH, &read_length);
        window[i].length = read_length;
      } else if (window[i].data[0] != '\0') memset(window[i].data, '\0', DATA_LENGTH);
    }
  } else {
    // If add/change to new packets for the whole window
    for (int i = 0; i < window_size; i++) {
      // Clear the previous buffer
      if (window[i].length != 0) {
        memset(window[i].data, '\0', DATA_LENGTH);
        window[i].length = 0;
      }
      if (!feof(fp)) {
        window[i].acked=0;
        read_file(fp, window[i].data, DATA_LENGTH, &read_length);
        window[i].length = read_length;
      } else if (window[i].data[0] != '\0') memset(window[i].data, '\0', DATA_LENGTH);
    }
  }
}

void send_window(struct window_data * window, int window_size, int send_start_index, int send_end_index, int head_seqNum,
  int sock, struct sockaddr_in * dest_attr, socklen_t dest_len, char * log) {
  struct timeval rt_start;
  if (send_start_index < 0 || send_end_index < 0 || send_start_index >= window_size || send_end_index >= window_size) {
    printf("Error: Invalid send index while send window");
    exit(1);
  }
  for (int i = send_start_index; i <= send_end_index; i++) {
  
    gettimeofday(&rt_start, NULL);
    window[i].rt_start = rt_start;
    send_data_message(window[i].data, window[i].length, head_seqNum + i, sock, dest_attr, dest_len, log);
  }
}

void send_file(char * input_file, int window_size, int sock, struct sockaddr_in * dest_attr, socklen_t dest_len, char * log) {
  struct window_data window[window_size];
  char data[DATA_LENGTH];
  int data_length;
  int head_seqNum = 0;
  FILE * fp = fopen(input_file, "r");

  set_window(fp, window, window_size, -1, -1);
  send_window(window, window_size, 0, window_size - 1, head_seqNum, sock, dest_attr, dest_len, log);

  char recv_buffer[PACKET_LENGTH];
  int recv_len;
  struct PacketHeader recv_header;
  struct timeval rt_start,rt_end;

  while (1) {
    // Try to receive ACK
    if (recv_len = recvfrom(sock, recv_buffer, PACKET_LENGTH, MSG_NOSIGNAL, (struct sockaddr *) dest_attr, &dest_len) == -1) {
      if (errno != EAGAIN && errno != EWOULDBLOCK) {
        perror("recvfrom");
        exit(1);
      }
    } else {
      get_header_info(recv_buffer, &recv_header);
      // Log for the received ACK
      FILE * log_fp = fopen(log, "a");
      fprintf(log_fp, "%u %u %u %u\n", recv_header.type, recv_header.seqNum, recv_header.length, recv_header.checksum);
      fclose(log_fp);
      if (recv_header.type == PACKET_TYPE_ACK) {
        printf("Received ACK from %s:%d, seqNum: %d\n", inet_ntoa(dest_attr->sin_addr), ntohs(dest_attr->sin_port), recv_header.seqNum);

        if (recv_header.seqNum >= head_seqNum && recv_header.seqNum < head_seqNum + window_size) {
          // If the packet seqNum is within the window
          // Set packet acked to true
          window[recv_header.seqNum - head_seqNum].acked = 1;
        }

        if (recv_header.seqNum == head_seqNum) {
          /* If receiving ACK with seqNum == head seqNum of the current window
             move the window forward such that the seqNum in the ACK becomes the new head seqNum */ 
          int next_seqNum = head_seqNum + window_size; // head_seqNum + 1;
          for (int i = 0; i < window_size; i++){
            if (window[i].acked != 1){
              next_seqNum = i + head_seqNum;
              printf("%d: %d\n", next_seqNum, window[i].acked);
              break;
            }
            // next_seqNum = head_seqNum + window_size;
          }
          printf("next_seq: %d\n",next_seqNum);

          // Window slides by pushing the unack packets to the front and append the new packets to the window
          set_window(
            fp, window, window_size,
            next_seqNum - head_seqNum < window_size ? next_seqNum - head_seqNum : -1, // set -1 to renew the whole window
            window_size - 1
          );
          if (window[0].length == 0) {
            // End of file
            break;
          }
          // Send out the newly added packets
          send_window(window, window_size, head_seqNum + window_size - next_seqNum, window_size -1,
            next_seqNum, sock, dest_attr, dest_len, log);
          // Update the head seqNum
          head_seqNum = next_seqNum;
          // Reset the retransmission timer on sliding window
          //gettimeofday(&rt_start, NULL);
        }
      }
    }

    for (int i = 0; i < window_size; i++){
      if (window[i].acked == 0){
        // If the sent packet has not been acked
	      //printf("%d :acked%d\n",i,window[i].acked);
        gettimeofday(&rt_end, NULL);
        double passed_time = (rt_end.tv_sec - window[i].rt_start.tv_sec) * 1000 + (rt_end.tv_usec - window[i].rt_start.tv_usec) / (double) 1000;
        // Retransmission timeout
        if (passed_time > RETRANSMISSION_TIME) {
          // If the counted time exceeds the retransmission time, resend the packet
          printf("Resend packet\n");
          
          send_data_message(window[i].data, window[i].length, head_seqNum + i, sock, dest_attr, dest_len, log);
          gettimeofday(&window[i].rt_start, NULL);
          // window[i].rt_start = rt_start;
        }
      }
    }
  }
  fclose(fp);
}

int main(int argc, const char **argv) {

  // Parse command line arguments
  // Retrieve configuration from argv
  if (argc != 6) {
    printf("Error: missing or extra arguments\n");
    return -1;
  }

  char * receiver_ip = (char *) argv[1];
  int receiver_port = atoi(argv[2]);
  int window_size = atoi(argv[3]);
  char * input_file = (char *) argv[4];
  char * log = (char *) argv[5];

  // -------

  FILE * log_fp;

  // Create or clear the log file
  if ((log_fp = fopen(log, "w")) == NULL) {
    printf("Error: Failure in opening the log file");
    return -1;
  }
  fclose(log_fp);

  // Create a UDP socket
  int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (sock == -1) {
    perror("socket");
    exit(1);
  }
  // Set socket to non-blocking
  fcntl(sock, F_SETFL, O_NONBLOCK);

  // Assign the receiver's socket address (si_receiver)
  struct sockaddr_in si_receiver;
  socklen_t slen = sizeof(si_receiver);
  memset((char *) &si_receiver, 0, sizeof(si_receiver));
  si_receiver.sin_family = AF_INET;
  si_receiver.sin_port = htons(receiver_port);
  struct hostent* sp = gethostbyname(receiver_ip);
  memcpy(&si_receiver.sin_addr, sp->h_addr, sp->h_length);

  // Send start message to initiate a connection to the receiver
  send_start_end_message_with_ack_recv(PACKET_TYPE_START, sock, &si_receiver, slen, log);

  printf("\n---The connection is started---\n\n");

  // Send the data messages to transimit the file

  send_file(input_file, window_size, sock, &si_receiver, slen, log);

  printf("\n---File transmission completes---\n\n");

  // Send end message to close the connection
  send_start_end_message_with_ack_recv(PACKET_TYPE_END, sock, &si_receiver, slen, log);

  printf("\n---The connection is ended---\n\n");

  // Close the UDP socket
  close(sock);

  return 0;
}
