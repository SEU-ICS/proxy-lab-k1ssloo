#include <stdio.h>
#include "csapp.h"
 
#define MAX_CACHE_SIZE 1049000
#define MAX_OBJECT_SIZE 102400
#define NUMBERS_OBJECT 10

static const char *user_agent_hdr = "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:10.0.3) Gecko/20120305 Firefox/10.0.3\r\n";
static const char *conn_hdr = "Connection: close\r\n";
static const char *proxy_hdr = "Proxy-Connection: close\r\n";

typedef struct {
    char *url;
    char *content;
    int *cnt; 
    int *is_used; 
} object;

static object *cache;
static int readcnt; 
static sem_t readcnt_mutex, writer_mutex; /* and the mutex that protects it */

/* helper function */
void doit(int client_fd);
void clienterror(int fd, char *cause, char *errnum, 
                 char *shortmsg, char *longmsg);
void parse_uri(char *uri, char *hostname, char *path, int *port);
void print_and_build_hdr(rio_t *rio_packet, char *new_request, char *hostname, char *port);
void *thread(void *varge_ptr);
void init_cache(void);
static void init_mutex(void);
int reader(int fd, char* url);
void writer(char* buf, char* url);

/* boot proxy as server get connfd from client */
int main(int argc, char **argv) 
{   
    init_cache();
    int listenfd, *connfd_ptr;
    char hostname[MAXLINE], port[MAXLINE];
    socklen_t clientlen;
    struct sockaddr_storage clientaddr;
    pthread_t tid;

    /* Check command line args */
    if (argc != 2) {
        fprintf(stderr, "usage: %s <port>\n", argv[0]);
        exit(1);
    }

    listenfd = Open_listenfd(argv[1]);
    while (1) {
        clientlen = sizeof(clientaddr);
        connfd_ptr = Malloc(sizeof(int)); /* alloc memory of each thread to avoid race */
        *connfd_ptr = Accept(listenfd, (SA *)&clientaddr, &clientlen); 
        Getnameinfo((SA *) &clientaddr, clientlen, hostname, MAXLINE, 
                    port, MAXLINE, 0);
        printf("Accepted connection from (%s, %s)\n", hostname, port);
        Pthread_create(&tid, NULL, thread, connfd_ptr);                                                                                    
    }
    return 0;
}

/*
 * Thread routine
 */
void *thread(void *varge_ptr){
    int connfd = *((int *)varge_ptr);
    Pthread_detach(pthread_self());
    doit(connfd);
    Free(varge_ptr);
    Close(connfd);
    return NULL;
}

/*
 * doit - handle one HTTP request/response transaction
 */
void doit(int client_fd) 
{
    int real_server_fd;
    char buf[MAXLINE], method[MAXLINE], url[MAXLINE], version[MAXLINE];
    char uri[MAXLINE], obj_buf[MAX_OBJECT_SIZE] = {0};  // Initialize obj_buf
    rio_t real_client, real_server;
    char hostname[MAXLINE], path[MAXLINE];
    int port;

    /* Read request line and headers from real client */
    Rio_readinitb(&real_client, client_fd);
    if (!Rio_readlineb(&real_client, buf, MAXLINE))     
        return;
    sscanf(buf, "%s %s %s", method, uri, version);
    strcpy(url, uri);       
    if (strcasecmp(method, "GET")) {                     
        clienterror(client_fd, method, "501", "Not Implemented",
                    "Tiny does not implement this method");
        return;
    }

    /* if object of request from cache */
    if (reader(client_fd, url)) {
        fprintf(stdout, "%s from cache\n", url);
        return;
    }

    /* prepare for parse uri and build new request */
    parse_uri(uri, hostname, path, &port);
    char port_str[10];
    sprintf(port_str, "%d", port); /* port from int convert to char */
    real_server_fd = Open_clientfd(hostname, port_str);  /* real server get fd from proxy(as client) */
    if (real_server_fd < 0) {
        printf("connection failed\n");
        return;
    }
    Rio_readinitb(&real_server, real_server_fd);
    
    char new_request[MAXLINE];
    sprintf(new_request, "GET %s HTTP/1.0\r\n", path);
    print_and_build_hdr(&real_client, new_request, hostname, port_str);

    /* proxy as client sent to web server */
    Rio_writen(real_server_fd, new_request, strlen(new_request));
    
    /* then proxy as server respond to real client */
    int char_nums;
    int obj_size = 0;
    while ((char_nums = Rio_readlineb(&real_server, buf, MAXLINE))) {
        Rio_writen(client_fd, buf, char_nums);

        /* prepare for write object to cache */
        if (obj_size + char_nums < MAX_OBJECT_SIZE) {
            strncat(obj_buf, buf, char_nums);  // Use strncat to append
            obj_size += char_nums;
        }
    }

    if (obj_size < MAX_OBJECT_SIZE) {
        obj_buf[obj_size] = '\0';  // Ensure obj_buf ends with a null terminator
        writer(obj_buf, url);
    }

    Close(real_server_fd);
}

/*
 * clienterror - returns an error message to the client
 */
void clienterror(int fd, char *cause, char *errnum, 
                 char *shortmsg, char *longmsg) 
{
    char buf[MAXLINE], body[MAXBUF];

    /* Build the HTTP response body */
    sprintf(body, "<html><title>Tiny Error</title>");
    sprintf(body, "%s<body bgcolor=""ffffff"">\r\n", body);
    sprintf(body, "%s%s: %s\r\n", body, errnum, shortmsg);
    sprintf(body, "%s<p>%s: %s\r\n", body, longmsg, cause);
    sprintf(body, "%s<hr><em>The Tiny Web server</em>\r\n", body);

    /* Print the HTTP response */
    sprintf(buf, "HTTP/1.0 %s %s\r\n", errnum, shortmsg);
    Rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "Content-type: text/html\r\n");
    Rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "Content-length: %d\r\n\r\n", (int)strlen(body));
    Rio_writen(fd, buf, strlen(buf));
    Rio_writen(fd, body, strlen(body));
}

/*
 * parse_uri - parse uri to get hostname, port, path from real client
 */
void parse_uri(char *uri, char *hostname, char *path, int *port) {
    *port = 80; /* default port */
    char* ptr_hostname = strstr(uri, "//");
    if (ptr_hostname) 
        ptr_hostname += 2; 
    else
        ptr_hostname = uri; 
    
    char* ptr_port = strstr(ptr_hostname, ":"); 
    if (ptr_port) {
        *ptr_port = '\0'; 
        strncpy(hostname, ptr_hostname, MAXLINE);
        sscanf(ptr_port + 1,"%d%s", port, path); 
    } else {
        char* ptr_path = strstr(ptr_hostname, "/");
        if (ptr_path) {
            *ptr_path = '\0';
            strncpy(hostname, ptr_hostname, MAXLINE);
            *ptr_path = '/';
            strncpy(path, ptr_path, MAXLINE);
            return;                               
        }
        strncpy(hostname, ptr_hostname, MAXLINE);
        strcpy(path, "");
    }
}

/*
 * print_and_build_hdr - print old request_hdr then build and print new request_hdr
 */
void print_and_build_hdr(rio_t *real_client, char *new_request, char *hostname, char *port) {
    char temp_buf[MAXLINE];

    /* print old request_hdr */
    while (Rio_readlineb(real_client, temp_buf, MAXLINE) > 0) {
        if (strstr(temp_buf, "\r\n")) break; 

        if (strstr(temp_buf, "Host:")) continue;
        if (strstr(temp_buf, "User-Agent:")) continue;
        if (strstr(temp_buf, "Connection:")) continue;
        if (strstr(temp_buf, "Proxy Connection:")) continue;

        sprintf(new_request, "%s%s", new_request, temp_buf);
    }

    /* build and print new request_hdr */
    sprintf(new_request, "%sHost: %s:%s\r\n", new_request, hostname, port);
    sprintf(new_request, "%s%s%s%s", new_request, user_agent_hdr, conn_hdr, proxy_hdr);
    sprintf(new_request, "%s\r\n", new_request);
}

/*
 * initialize the cache
 */
void init_cache(void) {
    init_mutex();
    readcnt = 0;
    
    /* cache is a Array of object */
    cache = (object*)Malloc(MAX_CACHE_SIZE);
    for (int i = 0; i < 10; i++) {
        cache[i].url = (char*)Malloc(sizeof(char) * MAXLINE);
        cache[i].content = (char*)Malloc(sizeof(char) * MAX_OBJECT_SIZE);
        cache[i].cnt = (int*)Malloc(sizeof(int));
        cache[i].is_used = (int*)Malloc(sizeof(int));
        *(cache[i].cnt) = 0;
        *(cache[i].is_used) = 0;
    }
}

/*
 * initialize the mutex
 */
static void init_mutex(void) {
    Sem_init(&readcnt_mutex, 0, 1);
    Sem_init(&writer_mutex, 0, 1);
}

/*
 * reader - read from cache to real client
 */
int reader(int fd, char* url) {
    int from_cache = 0; 

    P(&readcnt_mutex);
    readcnt++;
    if (readcnt == 1) 
        P(&writer_mutex);
    V(&readcnt_mutex);

    /* obj from cache then we should write content to fd of real client */
    for (int i = 0; i < NUMBERS_OBJECT; i++) {
        if (*(cache[i].is_used) && (strcmp(url, cache[i].url) == 0)) {
            from_cache = 1;
            Rio_writen(fd, cache[i].content, strlen(cache[i].content));
            *(cache[i].cnt)++;
            break;
        }
    }

    P(&readcnt_mutex);
    readcnt--;
    if (readcnt == 0) 
        V(&writer_mutex);
    V(&readcnt_mutex);

    return from_cache;        
}

/*
 * writer - write from real server to cache
 */
void writer(char* buf, char* url) {
    int min_cnt = *(cache[0].cnt);
    int insert_or_evict_i = 0;

    P(&writer_mutex);

    /* LRU: find the empty obj to insert or the obj of min cnt to evict */
    for (int i = 0; i < NUMBERS_OBJECT; i++) {
        if (*(cache[i].is_used) == 0) { 
            insert_or_evict_i = i;
            break;
        }
        if (*(cache[i].cnt) < min_cnt) { 
            insert_or_evict_i = i;
            min_cnt = *(cache[i].cnt);
        }
    }
    strcpy(cache[insert_or_evict_i].url, url);
    strcpy(cache[insert_or_evict_i].content, buf);
    *(cache[insert_or_evict_i].cnt) = 0;
    *(cache[insert_or_evict_i].is_used) = 1;

    V(&writer_mutex);
}
