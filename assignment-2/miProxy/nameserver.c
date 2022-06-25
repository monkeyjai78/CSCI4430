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
#define MAX_IP 20
#define MAX_CLIENTS 20

/*
  IP request message format:
  GET-IP\nclient-ip:10.0.0.1\n\n
*/

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

char * get_client_ip(char * buffer) {
  char * client_ip = (char *) malloc(sizeof(char) * 256);
  char * start_ptr = strstr(buffer, "client-ip:") + strlen("client-ip:");
  char * end_ptr = strstr(start_ptr, "\n");
  strncpy(client_ip, start_ptr, end_ptr - start_ptr);
  client_ip[end_ptr - start_ptr] = '\0';
  return client_ip;
}

int main(int argc, char *argv[]) {
  int server_socket, addrlen, activity, valread;
  char buffer[2097152];
  char ip_list[MAX_IP][64];
  int proxy_socks[MAX_CLIENTS];
  FILE * log_fptr;

  // Retrieve proxy configuration from argv
  if (argc != 5) {
    printf("Error: missing or extra arguments\n");
    return -1;
  }

  if (strcmp(argv[1], "--rr") != 0) {
    printf("Error: Invalid arguments\n");
    return -1;
  }

  int port = atoi(argv[2]);
  char * servers = argv[3];
  char * log = argv[4];
  int ipIndex = 0;

  // Retreive the list of servers' ip addresses from the the text file: servers
  FILE * servers_fptr = fopen(servers, "r");
  if (servers_fptr == NULL) {
  	printf("Error: Failure in opening the server ip list file");
    return -1;
  }
  int line_num = 0;
  while (fscanf(servers_fptr, "%s\n", ip_list[line_num]) != EOF) {
  	line_num++;
  }
  fclose(servers_fptr);

  // Create or clear the log file
  if ((log_fptr = fopen(log, "w")) == NULL) {
    printf("Error: Failure in opening the log file");
    return -1;
  }
  fclose(log_fptr);


  // Create socket of the proxy for listening to the client
  struct sockaddr_in address;
  server_socket = get_server_socket(&address, port);
  // accept the incoming connection
  addrlen = sizeof(address);
  // set of socket descriptors
  fd_set readfds;

  while (1) {
    // Clear the socket set
    FD_ZERO(&readfds);

    // Add connected sockets to the set
    FD_SET(server_socket, &readfds);
    for (int i = 0; i < MAX_CLIENTS; i++) {
      if (proxy_socks[i] != 0) {
        FD_SET(proxy_socks[i], &readfds);
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

      int new_proxy_socket = accept(server_socket, (struct sockaddr *)&address,
                              (socklen_t *)&addrlen);
      if (new_proxy_socket < 0) {
        perror("accept");
        exit(EXIT_FAILURE);
      }
      // Inform proxy of client socket number
      printf("\n---New client connection---\n");
      printf("socket fd is %d , ip is : %s , listen_port : %d \n", new_proxy_socket,
             inet_ntoa(address.sin_addr), ntohs(address.sin_port));

      // Add new proxy socket to the array of sockets
      for (int i = 0; i < MAX_CLIENTS; i++) {
        // If position is empty
        if (proxy_socks[i] == 0) {
          proxy_socks[i] = new_proxy_socket;
          break;
        }
      }
    }

    // Handling IO operation on the proxy's socket's activity
    // Client sockets
    for (int i = 0; i < MAX_CLIENTS; i++) {
      int sock = proxy_socks[i];
      if (sock != 0 && FD_ISSET(sock, &readfds)) {
      	// Do something
      	valread = recv(sock, buffer, 1024, 0);
        if (valread == 0) {
          // Somebody disconnected , get their details and print
          printf("\n---Host disconnected---\n");
          printf("Host disconnected , ip %s , port %d \n",
                 inet_ntoa(address.sin_addr), ntohs(address.sin_port));
          // Close the socket and mark as 0 in list for reuse
          close(sock);
          proxy_socks[i] = 0;
        } else {

          buffer[valread] = '\0';
          printf("\n---Received data---\n%s\n", buffer);

          if (strstr(buffer, "GET-IP")) {
            // If it's a request for retrieving a web server's ip
            printf("\n---New IP request---\n");
            printf("Received from: ip %s , port %d \n",
                   inet_ntoa(address.sin_addr), ntohs(address.sin_port));

            char * proxy_ip = inet_ntoa(address.sin_addr); // For logging
            char * client_ip = get_client_ip(buffer); // For logging

            // Use round-robin based load balancing scheme to determine the response_ip
            char * response_ip = ip_list[ipIndex];
            ipIndex = ipIndex + 1;
            if (ipIndex >= line_num){
              ipIndex = 0;
            }
            send(sock, response_ip, strlen(response_ip), 0);

            // Logging
            if ((log_fptr = fopen(log, "a")) == NULL) {
              printf("Error: Failure in opening the log file");
              return -1;
            }
            fprintf(
              log_fptr,
              "%s %s %s\n",
              proxy_ip, response_ip, client_ip
            );
            fclose(log_fptr);

            // Clear buffer
            buffer[0] = '\0';
      	  }
        }
      }
  	}
  }

  return 0;
}
