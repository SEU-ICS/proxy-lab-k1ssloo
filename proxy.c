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
}object;

static object *cache;
static int readcnt; 
static sem_t readcnt_mutex, writer_mutex; /* and the mutex that pretects it */

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
void writer(int* buf, char* url);

/* boot proxy as server get connfd from client*/
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
    return;
}

/*
 * doit - handle one HTTP request/response transaction
 */
void doit(int client_fd) 
{
    int real_server_fd;
    char buf[MAXLINE], method[MAXLINE], url[MAXLINE], version[MAXLINE];
    char uri[MAXLINE], obj_buf[MAXLINE];
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
    if(reader(client_fd, url)){
        fprintf(stdout, "%s from cache\n", url);
        return;
    }

    /* perpare for parse uri and build new request */
    parse_uri(uri, hostname, path, &port);
    char port_str[0];
    sprintf(port_str, "%d", port); /* port from int convert to char */
    real_server_fd = Open_clientfd(hostname, port_str);  /* real server get fd from proxy(as client) */
	if(real_server_fd < 0){
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
    while((char_nums = Rio_readlineb(&real_server, buf, MAXLINE))){
        Rio_writen(client_fd, buf, char_nums);

         /* perpare for write object to cache */
         if(obj_size + char_nums < MAX_OBJECT_SIZE){
            strcpy(obj_buf + obj_size, buf);
            obj_size += char_nums;
         }
    }

    if(obj_size < MAX_OBJECT_SIZE)
        writer(obj_buf, url);

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
    char* ptr_hostname = strstr(uri,"//");
    /* normal uri => http://hostname:port/path */
    /* eg. uri => http://www.cmu.edu:8080/hub/index.html */
    if (ptr_hostname) 
        /* hostname_eg1. uri => http://hostname... */
        ptr_hostname += 2; 
    else
        /* hostname_eg2. uri => hostname... <= NOT "http://"*/
        ptr_hostname = uri; 
    
    char* ptr_port = strstr(ptr_hostname, ":"); 
    /* port_eg1. uri => ...hostname:port... */
    if (ptr_port) {
        *ptr_port = '\0'; /* c-style: the end of string(hostname) is '\0' */
        strncpy(hostname, ptr_hostname, MAXLINE);

        /* change default port to current port */
        /* if path not char, sscanf will automatically store the ""(null) in the path */
        sscanf(ptr_port + 1,"%d%s", port, path); 
    } 
    /* port_eg1. uri => ...hostname... <= NOT ":port"*/
    else {
        char* ptr_path = strstr(ptr_hostname,"/");
        /* path_eg1. uri => .../path */
        if (ptr_path) {
            *ptr_path = '\0';
            strncpy(hostname, ptr_hostname, MAXLINE);
            *ptr_path = '/';
            strncpy(path, ptr_path, MAXLINE);
            return;                               
        }
        /* path_eg2. uri => ... <= NOT "/path"*/
        strncpy(hostname, ptr_hostname, MAXLINE);
        strcpy(path,"");
    }
    return;
}

/*
 * print_and_build_hdr - print old request_hdr then build and print new request_hdr
 */
void print_and_build_hdr(rio_t *real_client, char *new_request, char *hostname, char *port){
    char temp_buf[MAXLINE];

    /* print old request_hdr */
    while(Rio_readlineb(real_client, temp_buf, MAXLINE) > 0){
        if (strstr(temp_buf, "\r\n")) break; /* read to end */

        /* if all old request_hdr had been read, we print it */
        if (strstr(temp_buf, "Host:")) continue;
        if (strstr(temp_buf, "User-Agent:")) continue;
        if (strstr(temp_buf, "Connection:")) continue;
        if (strstr(temp_buf, "Proxy Connection:")) continue;

        sprintf(new_request, "%s%s", new_request, temp_buf);
    }

    /* build and print new request_hdr */
    sprintf(new_request, "%sHost: %s:%s\r\n", new_request, hostname, port);
    sprintf(new_request, "%s%s%s%s", new_request, user_agent_hdr, conn_hdr, proxy_hdr);
    sprintf(new_request,"%s\r\n", new_request);
}

/*
 * initialize the cache
 */
void init_cache(void){
    init_mutex();
    int readcnt = 0;
    
    /* cache is a Array of object*/
    cache = (object*)Malloc(MAX_CACHE_SIZE);
    for(int i = 0; i < 10; i++){
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
static void init_mutex(void){
    Sem_init(&readcnt_mutex, 0, 1);
    Sem_init(&writer_mutex, 0, 1);
}

/*
 * reader - read from cache to real client
 */
int reader(int fd, char* url){
    while(1){
        int from_cache = 0; /* equals 0 => obj not from cache; equals 1 => obj from cache */

        P(&readcnt_mutex);
        readcnt++;
        if(readcnt == 1) /* First in */
            P(&writer_mutex);
        V(&readcnt_mutex);

        /* obj from cache then we should write content to fd of real client */
        for(int i = 0; i < NUMBERS_OBJECT; i++){
            if(cache[i].is_used && (strcmp(url, cache[i].url) == 0)){
                from_cache = 1;
                Rio_writen(fd, cache[i].content, MAX_OBJECT_SIZE);
                *(cache[i].cnt)++;
                break;
            }
        }

        P(&readcnt_mutex);
        readcnt--;
        if(readcnt == 0) /* last out */
            V(&writer_mutex);
        V(&readcnt_mutex);

        return from_cache;        
    }
}

/*
 * writer - write from real server to cache
 */
void writer(int* buf, char* url){
    while(1){
        int min_cnt = *(cache[0].cnt);
        int insert_or_evict_i;

        P(&writer_mutex);

        /* LRU: find the empty obj to insert or the obj of min cnt to evict */
        for(int i = 0; i < NUMBERS_OBJECT; i++){
            if(*(cache[i].is_used) == 0){ /* insert */
                insert_or_evict_i = i;
                break;
            }
            if(*(cache[i].cnt) < min_cnt){ /* evict */
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
}
