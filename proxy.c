#include "csapp.h"

#define MAX_CACHE_SIZE 1049000
#define MAX_OBJECT_SIZE 102400
#define CACHE_OBJS_COUNT 10

static const char *user_agent_hdr =
    "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:10.0.3) "
    "Gecko/20120305 Firefox/10.0.3\r\n";

typedef struct {
    int valid;
    int size;
    int lru;
    char uri[MAXLINE];
    char obj[MAX_OBJECT_SIZE];
} cache_block;

typedef struct {
    cache_block blocks[CACHE_OBJS_COUNT];
    int time;
    sem_t mutex;
} cache_t;

cache_t cache;

void doit(int connfd);
void *thread(void *vargp);
int parse_uri(char *uri, char *hostname, char *path, char *port);
void build_header(char *header, char *hostname, char *path, char *port,
                  rio_t *client_rio);

void cache_init(void);
int cache_find(char *uri, char *buf, int *size);
void cache_insert(char *uri, char *buf, int size);

int main(int argc, char **argv)
{
    int listenfd;
    socklen_t clientlen;
    struct sockaddr_storage clientaddr;
    pthread_t tid;

    if (argc != 2) {
        fprintf(stderr, "usage: %s <port>\n", argv[0]);
        exit(1);
    }

    Signal(SIGPIPE, SIG_IGN);
    cache_init();

    listenfd = Open_listenfd(argv[1]);

    while (1) {
        int *connfdp = Malloc(sizeof(int));
        clientlen = sizeof(clientaddr);
        *connfdp = Accept(listenfd, (SA *)&clientaddr, &clientlen);
        Pthread_create(&tid, NULL, thread, connfdp);
    }

    return 0;
}

void *thread(void *vargp)
{
    int connfd = *((int *)vargp);

    Pthread_detach(Pthread_self());
    Free(vargp);

    doit(connfd);
    Close(connfd);

    return NULL;
}

void doit(int connfd)
{
    int serverfd;
    char buf[MAXLINE];
    char method[MAXLINE], uri[MAXLINE], version[MAXLINE];
    char hostname[MAXLINE], path[MAXLINE], port[MAXLINE];
    char header[MAXLINE * 2];
    char cache_buf[MAX_OBJECT_SIZE];
    rio_t client_rio, server_rio;
    ssize_t n;
    int cache_size;
    int obj_size = 0;
    int cacheable = 1;

    Rio_readinitb(&client_rio, connfd);

    if (Rio_readlineb(&client_rio, buf, MAXLINE) <= 0) {
        return;
    }

    if (sscanf(buf, "%s %s %s", method, uri, version) != 3) {
        return;
    }

    if (strcasecmp(method, "GET") != 0) {
        return;
    }

    if (cache_find(uri, cache_buf, &cache_size)) {
        Rio_writen(connfd, cache_buf, cache_size);
        return;
    }

    if (parse_uri(uri, hostname, path, port) < 0) {
        return;
    }

    build_header(header, hostname, path, port, &client_rio);

    serverfd = open_clientfd(hostname, port);
    if (serverfd < 0) {
        return;
    }

    Rio_writen(serverfd, header, strlen(header));

    Rio_readinitb(&server_rio, serverfd);
    while ((n = Rio_readnb(&server_rio, buf, MAXBUF)) > 0) {
        Rio_writen(connfd, buf, n);

        if (cacheable && obj_size + n <= MAX_OBJECT_SIZE) {
            memcpy(cache_buf + obj_size, buf, n);
            obj_size += n;
        } else {
            cacheable = 0;
        }
    }

    if (cacheable && obj_size > 0) {
        cache_insert(uri, cache_buf, obj_size);
    }

    Close(serverfd);
}

int parse_uri(char *uri, char *hostname, char *path, char *port)
{
    char *hostbegin;
    char *pathbegin;
    char *portbegin;
    char hostbuf[MAXLINE];

    if (strncasecmp(uri, "http://", 7) == 0) {
        hostbegin = uri + 7;
    } else {
        hostbegin = uri;
    }

    pathbegin = strchr(hostbegin, '/');
    if (pathbegin) {
        strcpy(path, pathbegin);
        snprintf(hostbuf, sizeof(hostbuf), "%.*s",
                 (int)(pathbegin - hostbegin), hostbegin);
    } else {
        strcpy(path, "/");
        strcpy(hostbuf, hostbegin);
    }

    portbegin = strchr(hostbuf, ':');
    if (portbegin) {
        *portbegin = '\0';
        strcpy(hostname, hostbuf);
        strcpy(port, portbegin + 1);
    } else {
        strcpy(hostname, hostbuf);
        strcpy(port, "80");
    }

    if (strlen(hostname) == 0 || strlen(port) == 0) {
        return -1;
    }

    return 0;
}

void build_header(char *header, char *hostname, char *path, char *port,
                  rio_t *client_rio)
{
    char buf[MAXLINE];
    char other_hdrs[MAXLINE];
    char host_hdr[MAXLINE];

    other_hdrs[0] = '\0';
    host_hdr[0] = '\0';

    while (Rio_readlineb(client_rio, buf, MAXLINE) > 0) {
        if (!strcmp(buf, "\r\n")) {
            break;
        }

        if (!strncasecmp(buf, "Host:", 5)) {
            strcpy(host_hdr, buf);
        } else if (!strncasecmp(buf, "User-Agent:", 11) ||
                   !strncasecmp(buf, "Connection:", 11) ||
                   !strncasecmp(buf, "Proxy-Connection:", 17)) {
            continue;
        } else {
            strcat(other_hdrs, buf);
        }
    }

    if (host_hdr[0] == '\0') {
        sprintf(host_hdr, "Host: %s:%s\r\n", hostname, port);
    }

    sprintf(header,
            "GET %s HTTP/1.0\r\n"
            "%s"
            "%s"
            "Connection: close\r\n"
            "Proxy-Connection: close\r\n"
            "%s"
            "\r\n",
            path, host_hdr, user_agent_hdr, other_hdrs);
}

void cache_init(void)
{
    int i;

    cache.time = 0;
    Sem_init(&cache.mutex, 0, 1);

    for (i = 0; i < CACHE_OBJS_COUNT; i++) {
        cache.blocks[i].valid = 0;
        cache.blocks[i].size = 0;
        cache.blocks[i].lru = 0;
        cache.blocks[i].uri[0] = '\0';
    }
}

int cache_find(char *uri, char *buf, int *size)
{
    int i;

    P(&cache.mutex);

    for (i = 0; i < CACHE_OBJS_COUNT; i++) {
        if (cache.blocks[i].valid && !strcmp(cache.blocks[i].uri, uri)) {
            cache.time++;
            cache.blocks[i].lru = cache.time;
            *size = cache.blocks[i].size;
            memcpy(buf, cache.blocks[i].obj, cache.blocks[i].size);
            V(&cache.mutex);
            return 1;
        }
    }

    V(&cache.mutex);
    return 0;
}

void cache_insert(char *uri, char *buf, int size)
{
    int i;
    int victim = 0;
    int min_lru;

    if (size > MAX_OBJECT_SIZE) {
        return;
    }

    P(&cache.mutex);

    for (i = 0; i < CACHE_OBJS_COUNT; i++) {
        if (!cache.blocks[i].valid) {
            victim = i;
            break;
        }
    }

    if (i == CACHE_OBJS_COUNT) {
        min_lru = cache.blocks[0].lru;
        victim = 0;

        for (i = 1; i < CACHE_OBJS_COUNT; i++) {
            if (cache.blocks[i].lru < min_lru) {
                min_lru = cache.blocks[i].lru;
                victim = i;
            }
        }
    }

    cache.time++;
    cache.blocks[victim].valid = 1;
    cache.blocks[victim].size = size;
    cache.blocks[victim].lru = cache.time;
    strcpy(cache.blocks[victim].uri, uri);
    memcpy(cache.blocks[victim].obj, buf, size);

    V(&cache.mutex);
}