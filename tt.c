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


#define MAX_EVENTS_NUMBER   (1024)
#define MAX_BUF_SIZE         (1024)
#define ACCESS_BUF           "quickbird_speedtest"
#define ACCESS_BUF_LEN      (sizeof(ACCESS_BUF) - 1)


#define set_nonblocking(fd)  fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK)

int UP_EXITED = 0;
int UP_CHILD = 0;
int PORT = 0;
int PID = 0;


typedef struct upload_connection_s{
    int                 fd;
    int                 len;
    int                 state;
    char                port[8];
    char                client_ip[16];
    struct timeval     begin_time;
    struct timeval     end_time;
}upload_connection_t;

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
                perror("accept");
                return -1;
            }
        }
  
        ret = getnameinfo((struct sockaddr *)&client_address, len \
                , hbuf, sizeof(hbuf), pbuf, sizeof(pbuf) \
                , NI_NUMERICHOST | NI_NUMERICSERV);
        if (ret) {
            close(connfd);
            continue;
        }
        h_len = strlen(hbuf);
        p_len = strlen(pbuf);
        if (h_len > 15 || p_len > 5) {
            close(connfd);
            continue;
        }
        
        upload_connection_t *info = calloc(1, sizeof(upload_connection_t));
        if (!info) {
            close(connfd);
            continue;
        }
        info->fd = connfd;
        gettimeofday(&(info->begin_time), NULL);
        memcpy(info->client_ip, hbuf, h_len);
        memcpy(info->port, pbuf, p_len);
        fprintf(stdout, "add a conn!\n");
        add_epoll_fd(epoll_fd, info);
    }
    return 0;

}

int read_fd_data(int sockfd, void *ptr)
{
    
    char buf[MAX_BUF_SIZE];
    upload_connection_t *s = ptr;
    while (1) {
        int ret = read(sockfd, buf, MAX_BUF_SIZE);
        if (ret < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            perror("read");
            return -1;
        } else if (ret == 0) {
            gettimeofday(&(s->end_time), NULL);
            fprintf(stdout, "end ip: %s port:%s diff time: %d len: %d\n", s->client_ip, s->port, 
                          (1000000 * s->end_time.tv_sec + s->end_time.tv_usec) - 
                          (1000000 * s->begin_time.tv_sec + s->begin_time.tv_usec), s->len);
            return 1;
        } else {
            if (s->state == 0) {
                if (ret < ACCESS_BUF_LEN) {
                    fprintf(stderr, "invalid read data.\n");
                    return -1;
                }
                if (memcmp(buf, ACCESS_BUF, ACCESS_BUF_LEN)) {
                    fprintf(stderr, "invalid read data.\n");
                    return -1;
                }
                s->state = 1;
            }
            s->len += ret;
        }
    }
    return 0;
}

void et(struct epoll_event *events, int num, int epoll_fd, int listen_fd)
{
    int i   = 0;
    int ret = 0;
    int sockfd = 0;
    upload_connection_t *s = NULL;
    
    for (i = 0; i < num; i ++) {
        s = events[i].data.ptr;
        sockfd = s->fd;
        if ((events[i].events & EPOLLERR) \
          ||(events[i].events & EPOLLHUP) \
          ||!(events[i].events & EPOLLIN))
        {
            fprintf (stderr, "epoll error\n");
            close (sockfd);
            free(s);
            
        } else if (sockfd == listen_fd) {
            ret = add_listen_fd(epoll_fd, listen_fd);
            if (ret) {
                fprintf(stderr, "accept fail.\n");
            }
        } else if (events[i].events & EPOLLIN) {
            ret = read_fd_data(sockfd, events[i].data.ptr);
            if (ret) {
                close(sockfd);
                free(s);
            }
        } 
    }
    
}


void sig_chld_handler(int signo)
{
    UP_CHILD = 1;
}


void signal_worker_process(int pid, int (*func)(void)) 
{
    int 	status = 0;
    int 	ret		= 0;
    if (UP_CHILD) {
        waitpid(pid, &status, 0);
        if (WIFEXITED(status)) {
            UP_EXITED = 1;
            return;
        }
        sleep(1);
        printf("re.\n");
        func();
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
    
    pid = fork();
    if (pid < 0) {
		fprintf(stderr, "fork fail");
        return -1;
    }
    else if (pid > 0) {
        exit(0);    
    }
    
    setsid();

    sa.sa_handler = sig_chld_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    if (sigaction(SIGCHLD, &sa, NULL) < 0)
        return -1;
 	
    if (chdir("/") < 0) {
		fprintf(stderr, "chdir fail");
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
		fprintf(stderr, "open fd 0 1 2 fail");
		return -1;
	}
	return 0;
	
}

int work_process()
{
    int ret = 0;
    struct sockaddr_in server;
    bzero(&server, sizeof(server));
    server.sin_family = AF_INET;
    server.sin_addr.s_addr = htonl(INADDR_ANY);
    server.sin_port = htons(PORT);
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        return -1;
    }
    int flg = 1;
    ret = setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &flg, sizeof(flg));
    if (ret == -1) {
        perror("setsockopt");
        return -1;
    }

    ret = bind(listen_fd, (struct sockaddr*)&server, sizeof(server));
    if (ret == -1) {
        perror("bind");
        return -1;
    }

    ret = listen(listen_fd, SOMAXCONN);
    if (ret == -1) {
        perror("listen");
        return -1;
    }

    struct epoll_event events[MAX_EVENTS_NUMBER];
    int epoll_fd = epoll_create(10);
    if (epoll_fd == -1) {
        perror("epoll_create");
        return -1;
    }
    
    upload_connection_t *info = calloc(1, sizeof(upload_connection_t));
    if (!info) {
        fprintf(stderr, "calloc err.\n");
        return -1;
    }
    info->fd = listen_fd;
    add_epoll_fd(epoll_fd, info);

    while (1) {
        ret = epoll_wait(epoll_fd, events, MAX_EVENTS_NUMBER, -1);
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
    return 0;
}

int start_work_process()
{
    int	pid = fork();
    if (pid < 0)  {
        fprintf(stderr, "create work process fail.\n");
        return -1;
    }
    else if (pid == 0) {
        work_process();
    } 
    PID = pid;
    return pid;
}


int main(int argc, char **argv) 
{
    char        c = 0;
    int         daemon = 0;
    while ((c = getopt(argc, argv, "p:d")) != -1) {
        switch (c) {
            case 'p':
                PORT = atoi(optarg);
                break;
            case 'd':
                daemon = 1;
                break;
        }
    }
    if (!PORT) {
        fprintf(stderr, "invalid port.\n");
        return -1;
    }
    if (daemon) {
        if (set_daemon()) {
            fprintf(stderr, "set daemon fail.\n");
            return -1;
        }
    }
    
    sigset_t	mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGCHLD);
    if (sigprocmask(SIG_BLOCK, &mask, NULL) == -1) {
        return -1;
    }
	
    sigemptyset(&mask);
    int pid = start_work_process();
    if (pid < 0) {
        return -1;
    }
	
    for (;;) {
        if (UP_EXITED) 
            break;
		
        sigsuspend(&mask); 
        signal_worker_process(PID, start_work_process);
    }
    return 0;
}

