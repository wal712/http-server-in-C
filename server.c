#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sendfile.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <pthread.h>

#define MESSAGE_Q 1024
#define MAXLINE 4096
#define ROOT_DIR "./www"
#define DEFAULT_FILE "index.html"

#define BAD_REQUEST_RESPONSE "HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n"
#define UNKNOWN_VERSION_RESPONSE "HTTP/1.1 505 HTTP Version Not Supported\r\nConnection: close\r\n\r\n"

enum Read_Status
{
    GOOD_HEADER,
    BAD_HEADER,
    TIMEOUT_OCCURED,
};

enum Http_Version
{
    NEW_VERSION,
    OLD_VERSION,
    UNKNOWN_VERSION,
};

typedef struct
{
    enum Http_Version version;
    int is_get;
    char path[MAXLINE];
    int keep_alive;
} Request_Info;

int readline(int fd, char *buf, int maxlen);
void *thread(void *vargp);
void echo(int connfd);
void handle_request(int connfd);

int main(int argc, char **argv)
{
    int socket_desc, port;
    struct sockaddr_in server, clientaddr;
    socklen_t clientlen = sizeof(clientaddr);
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
        client_sockp = malloc(sizeof(int)); // frees in thread function
        *client_sockp = accept(socket_desc, (struct sockaddr *)&clientaddr, &clientlen);
        pthread_create(&tid, NULL, thread, client_sockp);
    }
    printf("loop left\n");
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

/*
 * shamelessly modified this from: http://www.csc.villanova.edu/~mdamian/sockets/echoC.htm
 * read_request - attempts to read till request footer
 * return -1 if error
 * return 0 if proper footer
 */

int read_request(int fd, char *buf, int maxlen)
{
    int nc, n = 0;
    enum Read_Status status = BAD_HEADER;
    for (n = 0; n < maxlen - 1; n++)
    {
        nc = read(fd, &buf[n], 1);
        if (nc <= 0)
        {
            status = TIMEOUT_OCCURED;
            break;
        }
        if (n >= 4 && memcmp(&buf[n - 3], "\r\n\r\n", 4) == 0)
        {
            status = GOOD_HEADER;
            break; // Found the end of headers!
        }
    }
    buf[n + 1] = 0;
    return status;
}

// returns -1 for a malformed header (info isn't correct, or first header just doesn't exist), 0 for everything else
int parse_request(char *read_buf, Request_Info *info)
{
    // first, check http version, send error 505 if not recognized
    // Note: if http 1.0, can potentially skip check for connection field and just return, maybe set a flag
    char *header_pointer, *curr_header;
    const char *header_delim = "\r\n";

    curr_header = strtok_r(read_buf, header_delim, &header_pointer); // first header should be the main brunt of what we care about
    if (!curr_header)
    {
        // header doesn't even exist
        return -1;
    }

    char request_type[6];
    char temp_path[1024];
    char version[20];

    if (sscanf(curr_header, "%5s %1023s %19s", request_type, temp_path, version) != 3)
    {
        // improperly formatted header
        return -1;
    }

    const char *new_version = "HTTP/1.1";
    const char *old_version = "HTTP/1.0";

    if (strncmp(version, new_version, strlen(new_version)) == 0)
    {
        info->version = NEW_VERSION;
    }
    else if (strncmp(version, old_version, strlen(old_version)) == 0)
    {
        info->version = OLD_VERSION;
    }
    else
    {
        info->version = UNKNOWN_VERSION; // bad version, just return, don't need to parse the rest
        return 0;
    }

    if (strncmp(request_type, "GET", 3) == 0)
    {
        info->is_get = 1;
    }
    else
    {
        info->is_get = 0;
    }

    // parse filepath, prepend ./www and append index.html if path ends in '/'
    if (temp_path[strlen(temp_path) - 1] == '/')
    {
        snprintf(info->path, MAXLINE, "%s%s%s", ROOT_DIR, temp_path, DEFAULT_FILE);
    }
    else
    {
        snprintf(info->path, MAXLINE, "%s%s", ROOT_DIR, temp_path);
    }

    // check if 1.0, if so you can early return
    // otherwise we gotta check connection field, which will entail something like strtoking the buf and checking start of each header
    // if we find one starting with "Connection", check rest of field
    char *connect_start = "Connection";
    size_t connect_length = strlen(connect_start);
    while ((curr_header = strtok_r(NULL, header_delim, &header_pointer)))
    {
        if (strncmp(curr_header, connect_start, connect_length) == 0)
        {
            break;
        }
    }

    // If we find a valid header: we check if rest is either closed or keep alive
    char *keep_string = "Connection: Keep-alive";
    char *close_string = "Connection: close";
    if (curr_header)
    {
        // If they somehow botch the rest of the header, we treat it with same decision making as else statement below
        if (strncmp(curr_header, keep_string, strlen(keep_string)) == 0)
        {
            info->keep_alive = 1;
        }
        else if (strncmp(curr_header, close_string, strlen(close_string)) == 0)
        {
            info->keep_alive = 0;
        }
        else if (info->version == NEW_VERSION)
        {
            info->keep_alive = 1;
        }
        else
        {
            info->keep_alive = 0;
        }
    }
    else
    {
        // Didn't find connection header.
        // Our assumption changes depending on version
        if (info->version == NEW_VERSION)
        {
            info->keep_alive = 1;
        }
        else
        {
            info->keep_alive = 0;
        }
    }
    return 0;
}

/* thread routine */
void *thread(void *vargp)
{
    int connfd = *((int *)vargp);
    pthread_detach(pthread_self());
    free(vargp);
    handle_request(connfd);
    close(connfd);
    return NULL;
}

// no idea what to do if i can't determine file type, so i just say its text for now
const char *get_content_type(char *path)
{
    if (strstr(path, ".html"))
        return "text/html";
    if (strstr(path, ".png"))
        return "image/png";
    if (strstr(path, ".gif"))
        return "image/gif";
    if (strstr(path, ".jpg"))
        return "image/jpg";
    if (strstr(path, ".ico"))
        return "image/x-icon";
    if (strstr(path, ".css"))
        return "test/css";
    if (strstr(path, ".js"))
        return "application/javascript";
    return "text/plain";
}

void handle_request(int connfd)
{
    char buf[MAXLINE];
    char response_string[MAXLINE];

    switch (read_request(connfd, buf, MAXLINE))
    {
    case GOOD_HEADER:
        Request_Info request = {0};
        if (parse_request(buf, &request) == -1) // should fill out our request struct to use for constructing response headers
        {
            // malformed header found, just send bad request response
            send(connfd, BAD_REQUEST_RESPONSE, strlen(BAD_REQUEST_RESPONSE), MSG_NOSIGNAL); // no signal to prevent epipe crashing program
            // don't think i need to address the error, if client closes connection, we discover on next read and quit anyway
            // ya i realize that definitely isn't best practice, maybe I'll come back to it
            break;
        }

        char *ver_string;
        // check if http version is legit
        if (request.version == NEW_VERSION)
        {
            ver_string = "HTTP/1.1";
        }
        else if (request.version == OLD_VERSION)
        {
            ver_string = "HTTP/1.0";
        }
        else
        {
            // send unknown version response
            // I think we just hardcode connection close here and it's not a big deal

            send(connfd, UNKNOWN_VERSION_RESPONSE, strlen(UNKNOWN_VERSION_RESPONSE), MSG_NOSIGNAL);
            break;
        }

        char *conn_string;
        if (request.keep_alive)
        {
            conn_string = "Keep-alive";
        }
        else
        {
            conn_string = "close";
        }

        // check if GET request
        if (!request.is_get)
        {
            // send illegal operation response
            // IMPORTANT: we need to check connection status here, which means we can't leave early in parse function on illegal method
            snprintf(response_string, MAXLINE,
                     "%s 405 Method Not Allowed\r\n"
                     "Allow: GET\r\n"
                     "Connection: %s\r\n\r\n",
                     ver_string, conn_string);
            send(connfd, response_string, strlen(response_string), MSG_NOSIGNAL);
            break;
        }

        // check if path works / get file stats
        // happy path
        int fd = open(request.path, O_RDONLY);
        // check for open errors, send appropriate message if so
        if (fd == -1)
        {
            if (errno == ENOENT)
            {
                // send 404 response
                snprintf(response_string, MAXLINE,
                         "%s 404 Not Found\r\n"
                         "Connection: %s\r\n\r\n",
                         ver_string, conn_string);
            }
            else
            {
                // send permission issue response (403)
                snprintf(response_string, MAXLINE,
                         "%s 403 Forbidden\r\n"
                         "Connection: %s\r\n\r\n",
                         ver_string, conn_string);
            }
            // send here
            send(connfd, response_string, strlen(response_string), MSG_NOSIGNAL);
            break;
        }

        struct stat file_stat;
        if (fstat(fd, &file_stat) == -1)
        {
            // send permission issue response I guess
            snprintf(response_string, MAXLINE,
                     "%s 403 Forbidden\r\n"
                     "Connection: %s\r\n\r\n",
                     ver_string, conn_string);
            send(connfd, response_string, strlen(response_string), MSG_NOSIGNAL);
            break;
        }

        snprintf(response_string, MAXLINE,
                 "%s 200 OK\r\n"
                 "Content-Type: %s\r\n"
                 "Content-Length: %ld\r\n\r\n",
                 ver_string, get_content_type(request.path), file_stat.st_size);

        send(connfd, response_string, strlen(response_string), MSG_NOSIGNAL);
        sendfile(connfd, fd, NULL, (size_t)file_stat.st_size);
        // still check for sigpipe errno to break out of loop
        break;
    case BAD_HEADER:
        send(connfd, BAD_REQUEST_RESPONSE, strlen(BAD_REQUEST_RESPONSE), MSG_NOSIGNAL);
        break;
    case TIMEOUT_OCCURED:
        // technically client could have closed connection but im not making another status when the same thing happens either way
        break;
    }
    // char *to_send = "Hello World!\n";
    // write(connfd, to_send, sizeof(to_send));
}
