#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <netdb.h>
#include <sys/resource.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <stdarg.h>
#include <sys/stat.h>


#define MAX_EVENTS_NUM   (10240)
#define MAX_CONN_NUM     (2048)
#define MAX_FILENO_NUM   (20480)
#define MAX_BUF_SIZE         (4096)

#define MAX_LOG_SIZE      (1024 * 1024 * 600)

#define ACCESS_BUF           "quickbird_speedtest"
#define ACCESS_BUF_LEN      (sizeof(ACCESS_BUF) - 1)

#define set_nonblocking(fd)  fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK)

int UP_CHILD = 0;

int g_cpu_num = 0;
int g_pid_array[32] = {-1};

FILE *FILE_ERR = NULL;
FILE *FILE_ACCESS = NULL;
char *g_err_path = "/var/log/upload_error.log";
char *g_access_path = "/var/log/upload_access.log";

typedef struct upload_connection_s {
    int                 fd;
    int                 len;
    unsigned            trans_state:2;  /* 0: invalid; 1: success; 2:connection fail */
    unsigned            memory_state:1;
    char                port[8];
    char                client_ip[16];
    struct timeval     begin_time;
    struct timeval     end_time;
    struct upload_connection_s *next;
}upload_connection_t;


typedef struct upload_connection_pool_s {
    upload_connection_t *head;
    upload_connection_t *tail;
    int              free_num;
}upload_connection_pool_t; 

upload_connection_pool_t g_connection_pool;


int up_log_file_test(char *path, FILE **fp)
{
    struct stat   stat_info;
    int            size = 0;
    char           new_path[1024] = {0};
    int            ret = 0;
    FILE           *fp_new = NULL;

    if (!(*fp))
        goto AGAIN;

    if (access(path, F_OK)) {
        fclose(*fp);
        goto AGAIN;
    }
    if(stat(path, &stat_info) < 0){
        return -1;
    } else {
        size = stat_info.st_size;
    }

    if (size < MAX_LOG_SIZE)
        return 0;

    fclose (*fp);
    ret = snprintf(new_path, 1024, "%s.bak", path);
    if (ret < 0)
        return -1;
    if (rename(path, new_path))
        return -1;

AGAIN:
    fp_new = fopen(path, "a");
    if (!fp_new)
        return -1;
    *fp = fp_new;
    return 0;
}



void up_log(char *path, FILE *fp, char *fmt, ...)
{
    time_t          s;
    size_t          len = 0;
    char            buf[1024];
    char            *p = NULL;
    va_list         args;

    if (up_log_file_test(path, &fp)) {
        return;
    }

    s = time(NULL);
    ctime_r(&s, buf);

    len = strlen(buf);
    buf[len] = buf[len - 1] = '\t';

    p = buf + len + 1;
    va_start(args, fmt);
    vsprintf(p, fmt, args);
    va_end(args);

    fprintf(fp, "%s.\n", buf);
    fflush(fp);
}


int init_connection_pool()
{
    int     i = 0;
    upload_connection_t *root = NULL;

    root = calloc(MAX_CONN_NUM, sizeof(upload_connection_t));
    if (!root) {
        return -1;
    }
    g_connection_pool.head = &root[0];
    g_connection_pool.tail = &root[MAX_CONN_NUM - 1];
    g_connection_pool.free_num = MAX_CONN_NUM;
    for (i = MAX_CONN_NUM - 1; i > 0; i --) {
        root[i - 1].next = &root[i];
    }
    return 0;
}

upload_connection_t* get_connection()
{
    upload_connection_t *p = NULL;
    if (g_connection_pool.free_num) {
        p = g_connection_pool.head;
        g_connection_pool.head = p->next;
        g_connection_pool.free_num --;
        if (!g_connection_pool.free_num) 
            g_connection_pool.tail = NULL;
    } else {
        p = calloc(1, sizeof(upload_connection_t));
        if (!p) {
            return NULL;
        }
        p->memory_state = 1;
    }
    p->next = NULL;
    return p;
}

void free_connection(upload_connection_t *p)
{
    if (p->memory_state) {
        free(p);
        return;
    }
    memset(p, 0, sizeof(upload_connection_t));
    if (g_connection_pool.free_num) { 
        g_connection_pool.tail->next = p;
        g_connection_pool.tail = p;
    } else {
        g_connection_pool.head = p;
        g_connection_pool.tail = p;
    }
    g_connection_pool.free_num ++;
}

void add_epoll_fd(int epoll_fd, upload_connection_t *info) 
{
    struct epoll_event event;
    event.data.ptr = info;
    event.events = EPOLLIN | EPOLLET;
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, info->fd, &event);
    set_nonblocking(info->fd);
}



int add_listen_fd(int epoll_fd, int listen_fd)
{
    struct sockaddr_in client_address;
    socklen_t len = sizeof(struct sockaddr_in);
    char hbuf[NI_MAXHOST];
    char pbuf[NI_MAXSERV];
    int  h_len = 0;
    int  p_len = 0;
    int  ret = 0;

    for( ;; ) {
        int connfd = accept(listen_fd, (struct sockaddr*)&client_address, &len);
        if (connfd == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            } else {
                return -1;
            }
        }
  
        ret = getnameinfo((struct sockaddr *)&client_address, len \
                , hbuf, sizeof(hbuf), pbuf, sizeof(pbuf) \
                , NI_NUMERICHOST | NI_NUMERICSERV);
        if (ret) {
            up_log(g_err_path, FILE_ERR, "getnameinfo fail with errno:%d", errno);
            close(connfd);
            continue;
        }
        h_len = strlen(hbuf);
        p_len = strlen(pbuf);
        if (h_len > 15 || p_len > 5) {
            close(connfd);
            up_log(g_err_path, FILE_ERR, "%s", "invalid ip or port info");
            continue;
        }
        
        upload_connection_t *info = get_connection();
        if (!info) {
            close(connfd);
            up_log(g_err_path, FILE_ERR, "%s", "get connetion fail");
            continue;
        }
        info->fd = connfd;
        gettimeofday(&(info->begin_time), NULL);
        memcpy(info->client_ip, hbuf, h_len);
        memcpy(info->port, pbuf, p_len);
        add_epoll_fd(epoll_fd, info);
    }
    return 0;

}

int read_fd_data(upload_connection_t *s)
{
    char buf[MAX_BUF_SIZE];
    int  sockfd = s->fd;
    while (1) {
        int ret = read(sockfd, buf, MAX_BUF_SIZE);
        if (ret < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            up_log(g_err_path, FILE_ERR, "read fail with errno:%d", errno);
            s->trans_state = 2;
            return -1;
        } else if (ret == 0) {
            gettimeofday(&(s->end_time), NULL);
            fprintf(stdout, "end ip: %s port:%s diff time: %d len: %d\n", s->client_ip, s->port, 
                           s->len);
            return 1;
        } else {
            if (s->trans_state == 0) {
                if (ret < ACCESS_BUF_LEN) {
                    up_log(g_err_path, FILE_ERR, "%s", "invalid head len"); 
                    return -1;
                }
                if (memcmp(buf, ACCESS_BUF, ACCESS_BUF_LEN)) {
                    up_log(g_err_path, FILE_ERR, "%s", "invalid head data"); 
                    return -1;
                }
                s->trans_state = 1;
            }
            s->len += ret;
        }
    }
    return 0;
}

void et(struct epoll_event *events, int num, int epoll_fd, int listen_fd)
{
    int     i   = 0;
    int     ret = 0;
    int     sockfd = 0;
    double     last_time = 0;
    upload_connection_t *s = NULL;
    
    for (i = 0; i < num; i ++) {
        s = events[i].data.ptr;
        sockfd = s->fd;
        if ((events[i].events & EPOLLERR) \
          ||(events[i].events & EPOLLHUP) \
          ||!(events[i].events & EPOLLIN))
        {
            close (sockfd);
            up_log(g_err_path, FILE_ERR, "%s", "epoll connection error");
            free_connection(s);
            
        } else if (sockfd == listen_fd) {
            ret = add_listen_fd(epoll_fd, listen_fd);
            if (ret) {
                up_log(g_err_path, FILE_ERR, "accept fail with errno: %d", errno);
            }
        } else if (events[i].events & EPOLLIN) {
            ret = read_fd_data(s);
            if (ret) {
                last_time = (double)((1000000 * s->end_time.tv_sec + s->end_time.tv_usec) - 
                            (1000000 * s->begin_time.tv_sec + s->begin_time.tv_usec))/1000000;
                up_log(g_access_path, FILE_ACCESS \
                    ,  "[%d] IP:%s PORT:%s LAST_TIME:%f LEN:%d" \
                    , getpid(), s->client_ip, s->port, last_time, s->len);
                close(sockfd);
                free_connection(s);
            }
        } 
    }
    
}


void work_process(int listen_fd)
{
    int             ret = 0;
    struct epoll_event events[MAX_EVENTS_NUM];
    int epoll_fd = epoll_create(10);
    
    if (epoll_fd == -1) {
        up_log(g_err_path, FILE_ERR, "%s", "epoll_create fail");
        exit(2);
    }
    
    upload_connection_t *info = calloc(1, sizeof(upload_connection_t));
    if (!info) {
        up_log(g_err_path, FILE_ERR, "%s", "calloc fail");
        exit(2);
    }
    info->fd = listen_fd;
    add_epoll_fd(epoll_fd, info);

    ret = init_connection_pool();
    if (ret) {
        up_log(g_err_path, FILE_ERR, "%s", "alloc connection pool fail");
        exit(2);
    }

    while (1) {
        ret = epoll_wait(epoll_fd, events, MAX_EVENTS_NUM, -1);
        if (ret < 0) {
            if (ret == EINTR) {
                continue;
            }
            break;
        }
        et(events, ret, epoll_fd, listen_fd);
    }
    
    close(listen_fd);
    close(epoll_fd);
    free(info);
    exit(1);
}

int start_work_process(int listen_fd)
{
    int	pid = fork();
    if (pid < 0)  {
        up_log(g_err_path, FILE_ERR, "fork fail with errno: %d", errno); 
        return -1;
    }
    else if (pid == 0) {
        work_process(listen_fd);
    } 
    return pid;
}

void sig_chld_handler(int signo)
{
    UP_CHILD = 1;
}

void signal_worker_process(int listen_fd) 
{
    int 	status = 0;
    int 	pid		= 0;
    int     i       = 0;
    if (UP_CHILD) {
        for ( ;; ) {
            pid = waitpid(-1, &status, WNOHANG);
            if (pid == 0) {
                return;
            }
            if (pid == -1) {
                if (errno == EINTR) {
                    continue;
                }
                if (errno == ECHILD) {
                    return;
                }
            }

            for (i = 0; i < g_cpu_num; i ++) {
                if (g_pid_array[i] == -1) {
                    return;
                }
                if (pid == g_pid_array[i]) {
                    g_pid_array[i] = -1;
                    break;
                }
            }
            if (WTERMSIG(status)) {
                up_log(g_err_path, FILE_ERR, "process %d exit on signal %d" \
                    , pid, WTERMSIG(status));
            } else {
                up_log(g_err_path, FILE_ERR,  "process %d exit with return code %d" \
                    , pid, WEXITSTATUS(status));
            }
            pid = start_work_process(listen_fd);
            if (pid < 0) {
                return;
            }
            g_pid_array[i] = pid;
       }
    } 	
}


int set_daemon()
{	
    int i = 0;
    int	pid = 0;
    int fd = 0;
    int fd0, fd1, fd2;
    struct rlimit rl;
    struct sigaction sa;
        
    pid = fork();
    if (pid < 0) {
		fprintf(stderr, "fork fail");
        return -1;
    }
    else if (pid > 0) {
        exit(0);    
    }
    setsid();
    
    umask(0);
    
    if (getrlimit(RLIMIT_NOFILE, &rl) < 0) {
        fprintf(stderr, "getrlimit fail");
        return -1;
    }

    sa.sa_handler = SIG_IGN;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    if (sigaction(SIGHUP, &sa, NULL) < 0)
        return -1;

    sa.sa_handler = sig_chld_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    if (sigaction(SIGCHLD, &sa, NULL) < 0)
        return -1;

    if (chdir("/") < 0) {
		up_log(g_err_path, FILE_ERR, "%s", "chdir fail");
        return -1;
	}
    
    if (rl.rlim_max == RLIM_INFINITY)
        rl.rlim_max = 1024;
	
    for (i = 0; i < rl.rlim_max; i ++) 
        close(i);
    
    fd0 = open("/dev/null", O_RDWR);
    fd1 = dup(0);
    fd2 = dup(0);
    
    if (fd0 != 0 || fd1 != 1 || fd2 != 2) {
        up_log(g_err_path, FILE_ERR, "%s", "open fd 0 1 2 fail");
		return -1;
	}

    rl.rlim_cur = MAX_FILENO_NUM;
    rl.rlim_max = MAX_FILENO_NUM;
    if (setrlimit(RLIMIT_NOFILE, &rl) == -1) {
        up_log(g_err_path, FILE_ERR, "%s", "setrlimit fail");
        return -1;
    }
	return 0;
	
}

int main(int argc, char **argv) 
{
    int         i = 0;
    int         ret = 0;   
    int         port = 0;
    int         daemon = 0;
    char        c = 0;
    while ((c = getopt(argc, argv, "p:d")) != -1) {
        switch (c) {
            case 'p':
                port = atoi(optarg);
                break;
            case 'd':
                daemon = 1;
                break;
        }
    }
    if (!port) {
        fprintf(stderr, "invalid port.\n");
        return -1;
    }
    if (daemon) {
        if (set_daemon()) {
            fprintf(stderr, "set daemon fail.\n");
            return -1;
        }
    }
    FILE_ERR = fopen(g_err_path, "a");
    if (!FILE_ERR) {
        return -1;
    }
    FILE_ACCESS = fopen(g_access_path, "a");
    if (!FILE_ACCESS) {
        up_log(g_err_path, FILE_ERR, "%s", "fopen access log fail");
        return -1;
    }
    
    sigset_t	mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGCHLD);
    if (sigprocmask(SIG_BLOCK, &mask, NULL) == -1) {
        return -1;
    }

    struct sockaddr_in server;
    bzero(&server, sizeof(server));
    server.sin_family = AF_INET;
    server.sin_addr.s_addr = htonl(INADDR_ANY);
    server.sin_port = htons(port);
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        return -1;
    }
    int flg = 1;
    ret = setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &flg, sizeof(flg));
    if (ret == -1) {
        up_log(g_err_path, FILE_ERR, "setsockopt fail with errno: %d", errno);
        return -1;
    }

    ret = bind(listen_fd, (struct sockaddr*)&server, sizeof(server));
    if (ret == -1) {
        up_log(g_err_path, FILE_ERR, "bind fail with errno: %d", errno);
        return -1;
    }

    ret = listen(listen_fd, SOMAXCONN);
    if (ret == -1) {
        up_log(g_err_path, FILE_ERR, "listen fail with errno: %d", errno);
        return -1;
    }

    if (!daemon) {
        work_process(listen_fd);
        return 0;
    }

    g_cpu_num = sysconf(_SC_NPROCESSORS_ONLN);
    if (g_cpu_num == -1) {
        g_cpu_num = 1;
    }

    for (i = 0; i < g_cpu_num; i ++) {
        int pid = start_work_process(listen_fd);
        if (pid < 0) {
            return -1;
        }
        g_pid_array[i] = pid;
    }
    
    sigemptyset(&mask);
    for (;;) {		
        sigsuspend(&mask);
        signal_worker_process(listen_fd);
    }
    return 0;
}

