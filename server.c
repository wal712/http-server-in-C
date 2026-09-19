#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <pthread.h>

#define MESSAGE_Q 1024
#define MAXLINE 4096

int readline(int fd, char *buf, int maxlen);
void *thread(void *vargp);
void echo(int connfd);

int main(int argc, char **argv)
{
    int socket_desc, port;
    socklen_t clientlen;
    struct sockaddr_in server, clientaddr;
    int *client_sockp;
    pthread_t tid;

    if (argc != 2)
    {
        fprintf(stderr, "Usage: %s <port>\n", argv[0]);
        return 1;
    }

    port = atoi(argv[1]);

    socket_desc = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_desc == -1)
    {
        fprintf(stderr, "Failed to create socket\n");
        return 1;
    }

    server.sin_family = AF_INET;
    server.sin_addr.s_addr = INADDR_ANY;
    server.sin_port = htons((unsigned short)port);

    if (bind(socket_desc, (struct sockaddr *)&server, sizeof(server)) < 0)
    {
        fprintf(stderr, "Failed to bind socket\n");
        return 1;
    }

    if (listen(socket_desc, MESSAGE_Q) < 0)
    {
        fprintf(stderr, "Listening on socket failed\n");
        return 1;
    }

    while (1)
    {
        client_sockp = malloc(sizeof(int));
        *client_sockp = accept(socket_desc, (struct sockaddr *)&clientaddr, &clientlen);
        pthread_create(&tid, NULL, thread, client_sockp);
    }
}

/*
 * shamelessly took this from: http://www.csc.villanova.edu/~mdamian/sockets/echoC.htm
 * readline - read a line of text
 * return the number of characters read
 * return -1 if error
 */
int readline(int fd, char *buf, int maxlen)
{
    int nc, n = 0;
    for (n = 0; n < maxlen - 1; n++)
    {
        nc = read(fd, &buf[n], 1);
        if (nc <= 0)
            return nc;
        if (buf[n] == '\n')
            break;
    }
    buf[n + 1] = 0;
    return n + 1;
}

/* thread routine */
void *thread(void *vargp)
{
    int connfd = *((int *)vargp);
    pthread_detach(pthread_self());
    free(vargp);
    echo(connfd);
    close(connfd);
    return NULL;
}

void echo(int connfd)
{
    size_t n;
    char buf[MAXLINE];

    while ((n = readline(connfd, buf, MAXLINE)) != 0)
    {
        printf("server received %zu bytes\n", n);
        printf("%s", buf);
        write(connfd, buf, n);
    }
}
