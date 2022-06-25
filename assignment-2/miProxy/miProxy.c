#include <arpa/inet.h> //close
#include <stdio.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h> //strlen
#include <sys/socket.h>
#include <sys/time.h> //FD_SET, FD_ISSET, FD_ZERO, FD_SETSIZE macros
#include <sys/types.h>
#include <unistd.h> //close
#include <netdb.h>
#include <time.h>
#define MAX_CLIENTS 10
#define MAX_WEB_SERVERS 10
#define WEB_SERVER_PORT 80
#define MAX_BITRATE_OPTIONS 10

typedef struct client_conn client_conn;
typedef struct web_server_conn web_server_conn;

typedef struct client_conn {
  int sock;
  char * ip;
  int port;
  web_server_conn * web_ser_conn;
  int req_bitrate;
  char * req_chunk_name;
} client_conn;

typedef struct web_server_conn {
  int sock;
  char * ip;
  int port;
  client_conn * cli_conn;
  double T_cur;
  int remain_recv_buffer;
  int recving_vchunk;
  int recving_packet_size;
  struct timeval recv_starttime;
} web_server_conn;

int get_server_socket(struct sockaddr_in *address, int server_port) {
  int yes = 1;
  int server_socket;
  // create a master socket
  server_socket = socket(AF_INET, SOCK_STREAM, 0);
  if (server_socket <= 0) {
    perror("socket failed");
    exit(EXIT_FAILURE);
  }

  // set master socket to allow multiple connections ,
  // this is just a good habit, it will work without this
  int success =
      setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
  if (success < 0) {
    perror("setsockopt");
    exit(EXIT_FAILURE);
  }

  // type of socket created
  address->sin_family = AF_INET;
  address->sin_addr.s_addr = INADDR_ANY;
  address->sin_port = htons(server_port);

  // bind the socket to localhost port 8888
  success = bind(server_socket, (struct sockaddr *)address, sizeof(*address));
  if (success < 0) {
    perror("bind failed");
    exit(EXIT_FAILURE);
  }

  // try to specify maximum of 3 pending connections for the server socket
  if (listen(server_socket, 3) < 0) {
    perror("listen");
    exit(EXIT_FAILURE);
  }
  return server_socket;
}

int get_web_server_socket(struct sockaddr_in *address, char * www_ip, int web_server_port) {
  int web_ser_socket = socket(AF_INET, SOCK_STREAM, 0);
  address->sin_family = AF_INET;
  struct hostent *host = gethostbyname(www_ip);
  if (host == NULL) {
    return -1;
  }
  memcpy(&(address->sin_addr), host->h_addr, host->h_length);
  address->sin_port = htons(web_server_port);
  return web_ser_socket;
}

char * get_http_header_field(char *buffer, char *field_name) {
  char * field_val = (char *) malloc(sizeof(char) * 100);
  char search_key[50];
  strcpy(search_key, field_name);
  search_key[strlen(field_name)] = '\0';
  strcat(search_key, ": ");
  search_key[strlen(field_name) + strlen(": ")] = '\0';
  char * field_val_start = strstr(buffer, search_key);
  if (field_val_start == NULL) return NULL;
  field_val_start += strlen(search_key);
  char * field_val_end = strstr(field_val_start, "\r\n");
  strncpy(field_val, field_val_start, field_val_end - field_val_start);
  field_val[field_val_end - field_val_start] = '\0';
  return field_val;
}

int get_http_content_len(char * buffer) {
  //char * content_len_str = get_http_header_field(buffer, "Content-Length");
  char content_len_str[50];
  char * content_len_start = strstr(buffer, "Content-Length: ") + strlen("Content-Length: ");
  char * content_len_end = strstr(content_len_start, "\r\n");
  strncpy(content_len_str, content_len_start, content_len_end - content_len_start);
  // if (content_len_str == NULL) return -1;
  int content_len = atoi(content_len_str);
  return content_len;
}

int get_http_header_len(char * buffer) {
  char * content_len_end = strstr(buffer, "\r\n\r\n");
  int header_len = content_len_end + strlen("\r\n\r\n") - buffer;
  return header_len;
}

char * get_content_type(char * buffer) {
  char * content_type = get_http_header_field(buffer, "Content-Type");
  if (content_type == NULL) return NULL;
  return content_type;
}

char * get_video_chunk_name(char * buffer) {
  char * chunk_name = (char *) malloc(sizeof(char) * 100);
  char * chunk_name_start = strstr(buffer, "Seg");
  if (chunk_name_start == NULL) return NULL;
  while ((chunk_name_start - 1)[0] != '/') {
    if ((chunk_name_start - 1)[0] == '\n' || (chunk_name_start - 1)[0] == 0) return NULL;
    chunk_name_start--;
  }
  char * chunk_name_end = strstr(chunk_name_start, " ");
  strncpy(chunk_name, chunk_name_start, chunk_name_end - chunk_name_start);
  chunk_name[chunk_name_end - chunk_name_start] = '\0';
  return chunk_name;
}

void extract_bitrate_list(char * buffer, int rate_int[], int max_extracted_count) {
  char * ret = buffer;
  const char keyword[] = "rate=\"";
  char rate_string[10];
  int c = 0;
  while (1) {
    ret = strstr(ret, keyword);
    if (ret == NULL) break;
    ret = ret + 6;
    int j = 0;
    while (c < max_extracted_count) {
        if(ret[j] != '"'){
            rate_string[j] = ret[j];
            j++;
        }
        else {
          rate_string[j] = '\0';
          break;
        }
    }
    rate_int[c] = atoi(rate_string);
    c++;
  }
}

int sort_nzpos_int_arr(int int_arr[], int length) {
  int temp;
  for (int i = 0; i < length - 1; i++) {
    for (int j = i + 1; j < length; j++) {
      if (int_arr[i] == 0 || (int_arr[j] > 0 && int_arr[i] > int_arr[j])) {
        temp = int_arr[i];
        int_arr[i] = int_arr[j];
        int_arr[j] = temp;
      }
    }
  }
}

void modify_req_chunk_bitrate(char * buffer, int new_bitrate) {
  char * ret = strstr(buffer, "Seg");
  char new_buffer[10000];
  char tem[10000];
  int k = 0;
  int c = 0;
  while (1) {
    if ((ret - k)[0]=='/') {
        break;
    }
    k++;
  }
  while (1) {
      if (strcmp((buffer + c), (ret - k)) == 0) {
        break;
      } else {
          new_buffer[c] = (buffer + c)[0];
      }
      c++;
  }
  new_buffer[c] = '\0';
  
  sprintf(tem, "/%d%s", new_bitrate, ret);
  strcat(new_buffer, tem);
  strcpy(buffer, new_buffer);
  buffer[strlen(new_buffer)] = '\0';
}

//Select a suitable bit rate depending on T_cur
int select_bitrate(int bitrate_list[], double T_cur){
  int result = bitrate_list[0];
  int i = 1;
  while (bitrate_list[i] != 0 && i < MAX_BITRATE_OPTIONS) {
    if (T_cur >= bitrate_list[i] * 1.5) {
      result = bitrate_list[i];
    }
    i++;
  }
  return result;
}

void modify_req_manifest_nolist(char * buffer) {
  char new_buffer[10000];
  char * start_ptr = strstr(buffer, "big_buck_bunny");
  char * end_ptr = strstr(buffer, ".f4m");
  
  strncpy(new_buffer, buffer, start_ptr - buffer);
  new_buffer[start_ptr - buffer] = '\0';
  strcat(new_buffer, "big_buck_bunny_nolist");
  new_buffer[start_ptr - buffer + strlen("big_buck_bunny_nolist")] = '\0';
  strcat(new_buffer, end_ptr);
  new_buffer[(start_ptr - buffer) + strlen("big_buck_bunny_nolist") + (strlen(buffer) - (end_ptr - buffer))] = '\0';

  strcpy(buffer, new_buffer);
}

int main(int argc, char *argv[]) {
  int server_socket, addrlen, activity, valread;
  char buffer[2097152];
  int bitrate_list[MAX_BITRATE_OPTIONS] = {0};
  int is_bitrate_list_kept = 0;
  FILE * log_fptr;

  int use_dns = 0;
  int listen_port;
  double alpha;
  char * log;
  char * www_ip;
  char * dns_ip;
  int dns_port;

  // Retrieve proxy configuration from argv
  if ((argc != 7 && argc != 6) || (strcmp(argv[1], "--nodns") != 0 && strcmp(argv[1], "--dns")) != 0) {
    printf("Error: missing or extra arguments\n");
    return -1;
  }

  if (strcmp(argv[1], "--dns") == 0) use_dns = 1;

  if (use_dns) {
    listen_port = atoi(argv[2]);
    dns_ip = argv[3];
    dns_port = atoi(argv[4]);
    alpha = atof(argv[5]);
    log = argv[6];
  } else {
    listen_port = atoi(argv[2]);
    www_ip = argv[3];
    alpha = atof(argv[4]);
    log = argv[5];
  }

  if (alpha < 0 || alpha > 1) {
    printf("Error: Invalid arguments\n");
    return -1;
  }

  // Create or clear the log file
  if ((log_fptr = fopen(log, "w")) == NULL) {
    printf("Error: Failure in opening the log file");
    return -1;
  }
  fclose(log_fptr);

  // Declare socket-related variables
  client_conn cli_conns[MAX_CLIENTS];
  web_server_conn web_ser_conns[MAX_WEB_SERVERS];
  int client_sock = 0;
  int web_server_sock = 0;
  int conn_table [MAX_CLIENTS][MAX_WEB_SERVERS] = {0};
  int dns_sock;
  
  // initialization
  for (int i = 0; i < MAX_CLIENTS; i++) {
    cli_conns[i].sock = 0;
  }
  for (int i = 0; i < MAX_WEB_SERVERS; i++) {
    web_ser_conns[i].sock = 0;
    web_ser_conns[i].T_cur = -1;
  }

  // Create a socket of the proxy for listening to the client
  struct sockaddr_in address;
  server_socket = get_server_socket(&address, listen_port);
  // accept the incoming connection
  addrlen = sizeof(address);
  // set of socket descriptors
  fd_set readfds;

  if (use_dns) {
    // Build a socket connection with the DNS
    struct sockaddr_in dns_addr;
    dns_sock = get_web_server_socket(&dns_addr, dns_ip, dns_port);
    if (dns_sock < 0) {
      fprintf(stderr, "%s: unknown host\n", dns_ip);
      return -1;
    }
    // Connect to remote server
    if (connect(dns_sock, (struct sockaddr *) &dns_addr, sizeof(dns_addr)) == -1) {
      perror("Error connecting stream socket");
      return -1;
    }
  }

  while (1) {
    // Clear the socket set
    FD_ZERO(&readfds);

    // Add connected sockets to the set
    FD_SET(server_socket, &readfds);
    for (int i = 0; i < MAX_CLIENTS; i++) {
      client_sock = cli_conns[i].sock;
      if (client_sock != 0) {
        FD_SET(client_sock, &readfds);
      }
    }
    for (int i = 0; i < MAX_WEB_SERVERS; i++) {
      web_server_sock = web_ser_conns[i].sock;
      if (web_server_sock != 0) {
        FD_SET(web_server_sock, &readfds);
      }
    }
    // Wait for an activity on one of the sockets, timeout is NULL, so wait indefinitely
    printf("\n---Listening to sockets' activities---\n\n");
    activity = select(FD_SETSIZE, &readfds, NULL, NULL, NULL);
    if ((activity < 0) && (errno != EINTR)) {
      perror("select error");
    }

    // If something happened on the master socket,
    // then its an incoming connection, call accept()
    if (FD_ISSET(server_socket, &readfds)) {

      printf("Client connection###\n\n");

      int new_client_socket = accept(server_socket, (struct sockaddr *)&address,
                              (socklen_t *)&addrlen);
      if (new_client_socket < 0) {
        perror("accept");
        exit(EXIT_FAILURE);
      }
      // Inform proxy of client socket number
      printf("\n---New client connection---\n");
      printf("socket fd is %d , ip is : %s , listen_port : %d \n", new_client_socket,
             inet_ntoa(address.sin_addr), ntohs(address.sin_port));

      // Find the available positions in the arrays of client sockets and web server sockets
      int cli_conn_index = -1, web_ser_conn_index = -1;
      for (int i = 0; i < MAX_CLIENTS; i++) {
        // If position is empty
        if (cli_conns[i].sock == 0) {
          cli_conn_index = i;
          break;
        }
      }
      for (int i = 0; i < MAX_WEB_SERVERS; i++) {
        // if position is empty
        if (web_ser_conns[i].sock == 0) {
          web_ser_conn_index = i;
          break;
        }
      }

      // Build the socket connection only if there is an available socket slot
      if (cli_conn_index >= 0 && web_ser_conn_index >=0) {
        char assigned_www_ip[256];
        if (use_dns) {
          /*
            IP request message format:
            GET-IP\nclient-ip:10.0.0.1\n\n
          */
          printf("###DNS query###\n\n");
          char ip_request_msg[256] = "GET-IP\nclient-ip:";
          strcat(ip_request_msg, inet_ntoa(address.sin_addr));
          ip_request_msg[strlen("GET-IP\nclient-ip:") + strlen(inet_ntoa(address.sin_addr))] = '\0';
          strcat(ip_request_msg, "\n\n");
          ip_request_msg[strlen("GET-IP\nclient-ip:") + strlen(inet_ntoa(address.sin_addr)) + strlen("\n\n")] = '\0';
          // Send request to the DNS for retrieving a web sever's ip
          if (send(dns_sock, ip_request_msg, strlen(ip_request_msg), 0) < 0) {
            perror("Error sending data to the DNS");
          } else {
            // Receive a web server's ip from the DNS
            if ((valread = recv(dns_sock, buffer, 8192, 0)) < 0) {
              perror("Error receiving data from the DNS");
            }
            strcpy(assigned_www_ip, buffer);
            assigned_www_ip[valread] = '\0';
          }
        }

        // Create a connection to the web server
        struct sockaddr_in addr;
        int new_web_ser_socket;
        if (use_dns) {
          new_web_ser_socket = get_web_server_socket(&addr, assigned_www_ip, WEB_SERVER_PORT);
        } else {
          new_web_ser_socket = get_web_server_socket(&addr, www_ip, WEB_SERVER_PORT);
        }
        if (new_web_ser_socket < 0) {
          fprintf(stderr, "unknown host\n");
          return -1;
        }
        // Connect to remote server
        if (connect(new_web_ser_socket, (struct sockaddr *) &addr, sizeof(addr)) == -1) {
          perror("Error connecting stream socket");
          return -1;
        }

        char * ip;
        // Add new client socket to the array of client sockets
        cli_conns[cli_conn_index].sock = new_client_socket;
        ip = inet_ntoa(address.sin_addr);
        cli_conns[cli_conn_index].ip = (char *) malloc(sizeof(char)* strlen(ip));
        strcpy(cli_conns[cli_conn_index].ip, ip);
        cli_conns[cli_conn_index].port = ntohs(address.sin_port);
        cli_conns[cli_conn_index].web_ser_conn = &web_ser_conns[web_ser_conn_index];
        // Add new web server socket to the array of web server sockets
        web_ser_conns[web_ser_conn_index].sock = new_web_ser_socket;
        ip = inet_ntoa(addr.sin_addr);
        web_ser_conns[web_ser_conn_index].ip = (char *) malloc(sizeof(char)* strlen(ip));
        strcpy(web_ser_conns[web_ser_conn_index].ip, ip);
        web_ser_conns[web_ser_conn_index].port = ntohs(addr.sin_port);
        web_ser_conns[web_ser_conn_index].T_cur = -1;
        web_ser_conns[web_ser_conn_index].cli_conn = &cli_conns[cli_conn_index];

      } else {
        // Close the accepted socket if there is no available socket slot
        close(new_client_socket);
      }
    }

    // Handling IO operation on a client or web server socket
    // Client sockets

    for (int i = 0; i < MAX_CLIENTS; i++) {
      client_sock = cli_conns[i].sock;
      if (client_sock != 0 && FD_ISSET(client_sock, &readfds)) {

        // Receive the data from the client
        valread = recv(client_sock, buffer, 1024, 0);
        if (valread < 0) {
          perror("Error receiving client's data");
        } else if (valread == 0) {
          // Somebody disconnected , get their details and print
            printf("\n---Host disconnected---\n");
            printf("Host disconnected , ip %s , port %d \n\n",
                   cli_conns[i].ip,  cli_conns[i].port);
            // Close the socket and mark as 0 in list for reuse
            if (cli_conns[i].sock > 0) {
              close(cli_conns[i].sock);
              cli_conns[i].sock = 0;
            }
            if (cli_conns[i].web_ser_conn != NULL) {
              if (cli_conns[i].web_ser_conn->sock > 0)
              close(cli_conns[i].web_ser_conn->sock);
              cli_conns[i].web_ser_conn->sock = 0;
              cli_conns[i].web_ser_conn->T_cur = -1;
              cli_conns[i].web_ser_conn = NULL;
            }
            continue;
        }

        buffer[valread] = '\0';
        printf("\n---Http request from client, size: %d---\n", valread);
        printf("Received from: ip %s , port %d \n\n",
               cli_conns[i].ip, cli_conns[i].port);
        printf("%s\n", buffer);

        if (strstr(buffer, "big_buck_bunny.f4m")) {
          // If the client requests the video manifest
          
          if (!is_bitrate_list_kept) {
            // If there is no birate list kept yet
            
            // Send the original f4m file request for the f4m with the bitrate list
            if (cli_conns[i].web_ser_conn != NULL && send(cli_conns[i].web_ser_conn->sock, buffer, valread, 0) < 0) {
              perror("Error sending client's data to the web server");
            }
            // Receive the mainfest file with bitrate list, keep it and don't send back to the clients
            char manifest_buffer[65536];
            int total_manifest_valread = 0;
            int manifest_valread = recv(cli_conns[i].web_ser_conn->sock, manifest_buffer, 8192, 0);
            if (manifest_valread < 0) {
              perror("Error receiving web's server's data");
            } else if (manifest_valread == 0) {
              // Somebody disconnected , get their details and print
              printf("\n---Web server disconnected---\n\n");
              printf("Web server disconnected , ip %s , port %d \n\n",
                     web_ser_conns[i].ip,  web_ser_conns[i].port);
              // Close the socket and mark as 0 in list for reuse
              if (cli_conns[i].sock > 0) {
                close(cli_conns[i].sock);
                cli_conns[i].sock = 0;
              }
              if (cli_conns[i].web_ser_conn != NULL) {
                if (cli_conns[i].web_ser_conn->sock > 0)
                close(cli_conns[i].web_ser_conn->sock);
                cli_conns[i].web_ser_conn->sock = 0;
                cli_conns[i].web_ser_conn->T_cur = -1;
                cli_conns[i].web_ser_conn = NULL;
              }
              continue;
            } else {
              total_manifest_valread += manifest_valread;
              manifest_buffer[manifest_valread] = '\0';

              // Get the Http packet length for receiving remaining data of the Http package
              int header_len = get_http_header_len(manifest_buffer);
              int content_len = get_http_content_len(manifest_buffer);
              int http_packet_len = header_len + content_len;

              // Receive the remaining data
              while (total_manifest_valread < http_packet_len) {
                if ((manifest_valread = recv(cli_conns[i].web_ser_conn->sock, manifest_buffer + total_manifest_valread, 1024, 0)) < 0) {
                  perror("Error receiving web server's data");
                }
                total_manifest_valread += manifest_valread;
                if (manifest_valread == 0) {
                  // Somebody disconnected , get their details and print
                  printf("\n---Web server disconnected---\n\n");
                  printf("Web server disconnected , ip %s , port %d \n\n",
                         web_ser_conns[i].ip,  web_ser_conns[i].port);
                  // Close the socket and mark as 0 in list for reuse
                  if (cli_conns[i].sock > 0) {
                    close(cli_conns[i].sock);
                    cli_conns[i].sock = 0;
                  }
                  if (cli_conns[i].web_ser_conn != NULL) {
                    if (cli_conns[i].web_ser_conn->sock > 0)
                    close(cli_conns[i].web_ser_conn->sock);
                    cli_conns[i].web_ser_conn->sock = 0;
                    cli_conns[i].web_ser_conn->T_cur = -1;
                    cli_conns[i].web_ser_conn = NULL;
                  }
                  break;
                }
              }
              if (manifest_valread == 0) continue;

              extract_bitrate_list(manifest_buffer, bitrate_list, MAX_BITRATE_OPTIONS);
              sort_nzpos_int_arr(bitrate_list, MAX_BITRATE_OPTIONS);
              is_bitrate_list_kept = 1;
              printf("Bitrate list kept:");
              for (int j = 0; j < MAX_BITRATE_OPTIONS; j++) {
                printf(" %d", bitrate_list[j]);
              }
              printf("\n\n");
            }
          }
          
          // cli_conns[i].web_ser_conn->T_cur is initialized to be -1, which is < 0
          // For a new stream, set T_cur to the lowest available bitrate for the video
          if (cli_conns[i].web_ser_conn->T_cur < 0) {
            cli_conns[i].web_ser_conn->T_cur = bitrate_list[0];
          }

          // Modify the Http request for retrieving the f4m without the bitrate options
          modify_req_manifest_nolist(buffer);
          valread = strlen(buffer);
          printf("Modified Http request:\n%s\n", buffer);

        } else if (strstr(buffer,"Seg") && strstr(buffer,"Frag")) {
          // If the client requests a video chunk
          // Calculate the new bit rate and set the new_buffer 
          cli_conns[i].req_bitrate = select_bitrate(bitrate_list, cli_conns[i].web_ser_conn->T_cur);
          printf("@@@@@@ Selected bitrate: %d, T_cur: %f @@@@@@\n\n", cli_conns[i].req_bitrate, cli_conns[i].web_ser_conn->T_cur);

          modify_req_chunk_bitrate(buffer, cli_conns[i].req_bitrate);
          valread = strlen(buffer);
          printf("Modified Http request:\n%s\n", buffer);

          char * chunk_name = get_video_chunk_name(buffer);
          cli_conns[i].req_chunk_name = chunk_name;
          printf("@@@@@@ Video chunk name: %s @@@@@@\n\n", chunk_name);
        }

        // send the client's request to the web server
        if (cli_conns[i].web_ser_conn != NULL && send(cli_conns[i].web_ser_conn->sock, buffer, valread, 0) < 0) {
          perror("Error sending client's data to the web server");
        }
      } 
    }

    // Web server sockets
    for (int i = 0; i < MAX_WEB_SERVERS; i++) {
      web_server_sock = web_ser_conns[i].sock;
      if (web_server_sock != 0 && FD_ISSET(web_server_sock, &readfds)) {
        int max_valread;
        if (web_ser_conns[i].remain_recv_buffer > 0) {
          max_valread = web_ser_conns[i].remain_recv_buffer;
        } else {
          max_valread = 8192;
        }
        
        if (web_ser_conns[i].recving_packet_size <= 0) {
          // Stat of receiving package
          gettimeofday(&(web_ser_conns[i].recv_starttime), NULL);
        }

        // Receive the data from the web server which contains the Http header first
        valread = recv(web_server_sock, buffer, max_valread, 0);
        if (valread < 0) {
          perror("Error receiving web server's data");
        } else if (valread == 0) {
          if (cli_conns[i].sock > 0) {
            close(cli_conns[i].sock);
            cli_conns[i].sock = 0;
          }
          if (cli_conns[i].web_ser_conn != NULL) {
            if (cli_conns[i].web_ser_conn->sock > 0)
            close(cli_conns[i].web_ser_conn->sock);
            cli_conns[i].web_ser_conn->sock = 0;
            cli_conns[i].web_ser_conn->T_cur = -1;
            cli_conns[i].web_ser_conn = NULL;
          }
        }
        buffer[valread] = '\0';

        if (web_ser_conns[i].remain_recv_buffer > 0) {
          web_ser_conns[i].remain_recv_buffer -= valread;
        }

        if (strstr(buffer, "Content-Length:") != NULL) {
          // If ccontains header

          // Get the Http packet length for receiving remaining data of the Http package
          int header_len = get_http_header_len(buffer);
          int content_len = get_http_content_len(buffer);
          int http_packet_len = header_len + content_len;
          web_ser_conns[i].remain_recv_buffer = http_packet_len - valread;

          // Extract the content type of the Http response packet
          char * content_type = get_content_type(buffer);
          printf("@@@@@@ Content-type: %s @@@@@@\n", content_type);

          web_ser_conns[i].recving_packet_size = http_packet_len;

          if (strcmp(content_type, "video/f4f") == 0) {
            // If the content type is video/f4f, it is probably the video chunk
            web_ser_conns[i].recving_vchunk = 1;
          } else {
            web_ser_conns[i].recving_vchunk = 0;
          }
        }

        if (web_ser_conns[i].recving_packet_size > 0 && web_ser_conns[i].remain_recv_buffer <= 0) {
          // Complete receiving an entire http packet
          if (web_ser_conns[i].recving_vchunk) {
            struct timeval recv_endtime;
            gettimeofday(&recv_endtime, NULL);
            // Calculate the Throughput
            double recv_duration = recv_endtime.tv_sec - web_ser_conns[i].recv_starttime.tv_sec
              + (recv_endtime.tv_usec - web_ser_conns[i].recv_starttime.tv_usec) / (double) 1000000;
            double T_new = 0;
            T_new = ((double)web_ser_conns[i].recving_packet_size * 8 / 1000) / recv_duration;
            web_ser_conns[i].T_cur = alpha * T_new + (1 - alpha) * web_ser_conns[i].T_cur;

            printf("@@@@@@ Chunk receival completed, Duration: %fs, Throughput: %.2fKbps @@@@@@\n\n", recv_duration, T_new);

            // Print the log to the log file
            if ((log_fptr = fopen(log, "a")) == NULL) {
              printf("Error: Failure in opening the log file");
              return -1;
            }
            fprintf(
              log_fptr,
              "%s %s %s %f %f %f %d\n",
              web_ser_conns[i].cli_conn->ip, web_ser_conns[i].cli_conn->req_chunk_name, web_ser_conns[i].ip,
              recv_duration, T_new, web_ser_conns[i].T_cur, web_ser_conns[i].cli_conn->req_bitrate
            );
            fclose(log_fptr);
          }

          web_ser_conns[i].remain_recv_buffer = 0;
          web_ser_conns[i].recving_packet_size = 0;
          web_ser_conns[i].recving_vchunk = 0;
        }

        printf("\n---Http response from web server, size: %d---\n", valread);
        printf("Received from: ip %s , port %d \n\n",
               web_ser_conns[i].ip, web_ser_conns[i].port);

        // send the web server's response to the client
        if (web_ser_conns[i].cli_conn != NULL && send(web_ser_conns[i].cli_conn->sock, buffer, valread, 0) < 0) {
          perror("Error sending web server's data to the client");
        }
      }
    }
  }

  return 0;
}
