/*
    Simple udp client
*/
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netdb.h>

// #define SERVER "localhost"
#define BUFLEN 512
#define PORT 8888

void die(const char *s)
{
    perror(s);
    exit(1);
}

int send_message(const char *server)
{
    struct sockaddr_in si_other;
    int s;
    socklen_t slen = sizeof(si_other);
    char buf[BUFLEN];
    char message[BUFLEN];
    sprintf(message, "Hello World!");

    if ((s=socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) == -1)
    {
        die("socket");
    }

    memset((char *) &si_other, 0, sizeof(si_other));
    si_other.sin_family = AF_INET;
    si_other.sin_port = htons(PORT);
    struct hostent* sp = gethostbyname(server);
    memcpy(&si_other.sin_addr, sp->h_addr, sp->h_length);

    //if (inet_aton(SERVER , &si_other.sin_addr) == 0)
    //{
    //    fprintf(stderr, "inet_aton() failed\n");
    //    exit(1);
    //}

    //send the message
    if (sendto(s, message, strlen(message) , 0 , (struct sockaddr *) &si_other, slen)==-1)
    {
        die("sendto()");
    }

    //receive a reply and print it
    //clear the buffer by filling null, it might have previously received data
    memset(buf,'\0', BUFLEN);
    //try to receive some data, this is a blocking call
    sleep(3);
    if (recvfrom(s, buf, BUFLEN, 0, (struct sockaddr *) &si_other, &slen) == -1)
    {
        die("recvfrom()");
    }
    printf("UDP received packet from %s:%d\n", inet_ntoa(si_other.sin_addr), ntohs(si_other.sin_port));
    printf("Data: %s\n" , buf);

    close(s);
    return 0;
}

int main(int argc, const char **argv) {
    // Parse command line arguments
    if (argc != 2) {
        printf("Usage: ./client hostname port_num message\n");
        return 1;
    }
    const char *hostname = argv[1];

    
    if (send_message(hostname) == -1) {
        return 1;
    }

    return 0;
}
