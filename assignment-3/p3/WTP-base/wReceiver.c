#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>

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

int validate_checksum(char * buffer, int data_length, unsigned int recv_checksum) {
  unsigned int cal_checksum = crc32(buffer + HEADER_LENGTH, data_length);
  if (recv_checksum == cal_checksum) {
    return 1;
  }
  else return 0;
}

void send_ack(int seqNum, int sock, const struct sockaddr * dest_attr, socklen_t dest_len, char * log) {
  char header_buffer[HEADER_LENGTH];

  struct PacketHeader header;
  header.type = PACKET_TYPE_ACK;
  header.seqNum = seqNum;
  header.length = 0;
  header.checksum = 0;

  convert_header_to_bytes(&header, header_buffer);

  if (sendto(sock, header_buffer, HEADER_LENGTH, MSG_NOSIGNAL, dest_attr, dest_len) == -1) {
    perror("sendto()");
    exit(1);
  } else {
    // Log for the sent ACK
    FILE * log_fp = fopen(log, "a");
    fprintf(log_fp, "%u %u %u %u\n", header.type, header.seqNum, header.length, header.checksum);
    fclose(log_fp);
  }
}

void set_window(struct window_data * window, int window_size, int keep_start_index, int keep_end_index) {
  if (keep_start_index >= 0 && keep_end_index >= 0 &&
    (keep_start_index > keep_end_index || keep_start_index >= window_size || keep_end_index >= window_size)) {
    printf("Error: Invalid keep index while set window");
    exit(1);
  }
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
    }
  } else {
    // If renew the whole window
    for (int i = 0; i < window_size; i++) {
      // Clear the previous buffer
      if (window[i].length != 0) {
        memset(window[i].data, '\0', DATA_LENGTH);
        window[i].length = 0;
      }
    }
  }
}

void write_window(FILE * fp, struct window_data * window, int window_size, int write_start_index, int write_end_index) {
  if (write_start_index >= 0 && write_end_index >= 0 &&
    (write_start_index > write_end_index || write_start_index > window_size || write_end_index > window_size ||
      write_start_index < 0 || write_end_index < 0)) {
    printf("Error: Invalid write index while write window");
    exit(1);
  }
  for (int i = write_start_index; i <= write_end_index; i++) {
    fwrite(window[i].data, window[i].length, 1, fp);
  }
}

int main(int argc, const char **argv) {

  // Parse command line arguments
  // Retrieve configuration from argv
  if (argc != 5) {
    printf("Error: missing or extra arguments\n");
    return -1;
  }

  int port_num = atoi(argv[1]);
  int window_size = atoi(argv[2]);
  char * output_dir = (char *) argv[3];
  char * log = (char *) argv[4];

  // ---------

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

  // Assign the receiver's socket address (si_me)
  struct sockaddr_in si_me, si_sender;
  socklen_t slen = sizeof(si_sender);

  memset((char *) &si_me, 0, sizeof(si_me));
  si_me.sin_family = AF_INET;
  si_me.sin_port = htons(port_num);
  si_me.sin_addr.s_addr = htonl(INADDR_ANY);

  // Bind the socket to the port of the receiver
  if (bind(sock, (struct sockaddr*) &si_me, sizeof(si_me)) == -1) {
    perror("bind");
    exit(1);
  }

  char buffer[PACKET_LENGTH];
  int connection = 0;

  while (1) {
    // One loop per each connection

    char output_file[100];
    sprintf(output_file, "%s/FILE-%d.out", output_dir, connection);
    FILE * fp;

    struct window_data window[window_size];
    struct PacketHeader recv_header;
    int in_connection = 0, head_seqNum = 0, recv_len;

    // Initialize the window
    set_window(window, window_size, -1, -1);

    // Keep listening for data
    while(1) {
      printf("\n---Waiting for data on port %d---\n", port_num);
      fflush(stdout);

      // Try to receive the packet
      if ((recv_len = recvfrom(sock, buffer, PACKET_LENGTH, MSG_NOSIGNAL, (struct sockaddr *) &si_sender, &slen)) == -1) {
        perror("recvfrom()");
        exit(1);
      } else {
        // Print details of the sender and the data received
        printf("Received data from %s:%d\n", inet_ntoa(si_sender.sin_addr), ntohs(si_sender.sin_port));
        get_header_info(buffer, &recv_header);
        printf("Packet type: %d, total length: %d, data length: %d, seqNum: %d, checksum: %d\n",
          recv_header.type, recv_len, recv_header.length, recv_header.seqNum, recv_header.checksum);
        // Log for the received packet
        log_fp = fopen(log, "a");
        fprintf(log_fp, "%u %u %u %u\n", recv_header.type, recv_header.seqNum, recv_header.length, recv_header.checksum);
        fclose(log_fp);

        // On receiving START and END messages
        if (recv_header.type == PACKET_TYPE_START || recv_header.type == PACKET_TYPE_END) {
          // Send back the ACK with the same random seqNum
          send_ack(recv_header.seqNum, sock, (struct sockaddr *) &si_sender, slen, log);
          if (recv_header.type == PACKET_TYPE_START) {
            printf("\n---The connection is started---\n");
            // Open the output file on connection built
            if (in_connection == 0) fp = fopen(output_file, "w+");
            in_connection = 1;
          }
          else if (recv_header.type == PACKET_TYPE_END) {
            printf("\n---The connection is ended---\n\n");
            in_connection = 0;
            break;
          }
        }

        // On receiving DATA messages
        if (in_connection && recv_header.type == PACKET_TYPE_DATA) {
          int isPacketValid = validate_checksum(buffer, recv_header.length, recv_header.checksum);

          if (recv_header.seqNum >= head_seqNum && recv_header.seqNum < head_seqNum + window_size && isPacketValid) {
            /* If the received seqNum is within the window and
               the packet is not corrupted (i.e. received and calculated checksum match each other) */

            // Keep the received packet
            memcpy(window[recv_header.seqNum - head_seqNum].data, buffer + HEADER_LENGTH, recv_header.length);
            window[recv_header.seqNum - head_seqNum].length = recv_header.length;
          }

          if (recv_header.seqNum < head_seqNum + window_size  && isPacketValid) {
            //If the received packet is not to be dropped
            
            // Find the next expected seqNum
            int next_expected_seqNum = -1;
            for (int i = 0; i < window_size; i++) {
              if (window[i].length == 0) {
                // window[i].length == 0 implies the packet refering to (i + 1)th slot of the window is empty and thus not received
                // Assign the next expected seqNum to be that of the first unreceived packet in the current window
                next_expected_seqNum = head_seqNum + i;
                break;
              }
            }

            // assign next expected seqNum to be head_seqNum + window_size
            if (next_expected_seqNum == -1) next_expected_seqNum = head_seqNum + window_size;

            if (next_expected_seqNum > head_seqNum) {
              /* If the next expected seqNum is behind the current head seqNum
                 (i.e. packets with seqNum < next expected seqNum are all received),
                 move the window forward such that the next expected seqNum become the new head seqNum */

              // Write the data of those packets which are going to slide out from the window
              write_window(fp, window, window_size, 0, next_expected_seqNum - head_seqNum - 1);
              // Move the window forward
              set_window(
                window,
                window_size,
                next_expected_seqNum - head_seqNum < window_size ? next_expected_seqNum - head_seqNum : -1, // set -1 to renew the whole window
                window_size - 1
              );
              // Update the head seqNum
              head_seqNum = next_expected_seqNum;
            }

            // Send back the ACK if the received checksum of the received packet is valid
            send_ack(next_expected_seqNum, sock, (struct sockaddr *) &si_sender, slen, log);
            printf("Sending ACK to %s:%d, seqNum: %d\n", inet_ntoa(si_sender.sin_addr), ntohs(si_sender.sin_port), recv_header.seqNum + 1);
          }
        }
      }
    }

    // Close the output file
    fclose(fp);

    connection++;
  }

  // Close the UDP socket
  close(sock);

  return 0;
}
