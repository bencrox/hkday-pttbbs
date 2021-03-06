#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <ctype.h>
#include <assert.h>
#include <errno.h>

#include "cmsys.h"

#ifndef DEFAULT_TCP_QLEN
#define DEFAULT_TCP_QLEN    (10)
#endif 
#ifndef DEFAULT_IOWRITE_EAGAIN_WAIT_MICROSECOND
#define DEFAULT_IOWRITE_EAGAIN_WAIT_MICROSECOND	(10)
#endif

uint32_t
ipstr2int(const char *ip)
{
    unsigned int i;
    uint32_t val = 0;
    char buf[32];
    char *nil, *p;

    strlcpy(buf, ip, sizeof(buf));
    p = buf;
    for (i = 0; i < 4; i++) {
	nil = strchr(p, '.');
	if (nil != NULL)
	    *nil = 0;
	val *= 256;
	val += atoi(p);
	if (nil != NULL)
	    p = nil + 1;
    }
    return val;
}

// addr format:
// xxx.xxx.xxx.xxx:port
// :port  (bind to loopback)
// *:port (bind to addr_any, allow remote connect)
// all others formats are UNIX domain socket path.

int tobindex(const char *addr, int qlen, int (*_setsock)(int), int do_listen)
{
    const int v_on = 1;
    int     sockfd;

    assert(addr && *addr);

    if (!isdigit(addr[0]) && addr[0] != ':' && addr[0] != '*') {
	struct sockaddr_un servaddr;

	if ( (sockfd = socket(PF_UNIX, SOCK_STREAM, 0)) < 0 ) {
	    perror("socket()");
	    exit(1);
	}

	servaddr.sun_family = AF_UNIX;
	strlcpy(servaddr.sun_path, addr, sizeof(servaddr.sun_path));

	setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR,
		   (void *)&v_on, sizeof(v_on));

	if (_setsock)
	    _setsock(sockfd);

	// remove the file first if it exists.
	unlink(servaddr.sun_path);

	if (bind(sockfd, (struct sockaddr *)&servaddr, sizeof(servaddr)) < 0) {
	    perror("bind()");
	    exit(1);
	}
    }
    else {
	char buf[64], *port;
	struct sockaddr_in servaddr;

	if ( (sockfd = socket(PF_INET, SOCK_STREAM, 0)) < 0 ) {
	    perror("socket()");
	    exit(1);
	}

	strlcpy(buf, addr, sizeof(buf));
	if ( (port = strchr(buf, ':')) != NULL)
	    *port++ = '\0';

	assert(port && atoi(port) != 0);

	setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR,
		   (void *)&v_on, sizeof(v_on));

	if (_setsock)
	    _setsock(sockfd);

	if (!buf[0])
	    servaddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	else if (buf[0] == '*')
	    servaddr.sin_addr.s_addr = htonl(INADDR_ANY); // XXX use INADDR_LOOPBACK?
	else if (inet_aton(buf, &servaddr.sin_addr) == 0) {
	    perror("inet_aton()");
	    exit(1);
	}

	servaddr.sin_port = htons(atoi(port));
	servaddr.sin_family = AF_INET;

	if( bind(sockfd, (struct sockaddr *)&servaddr, sizeof(servaddr)) < 0 ) {
	    perror("bind()");
	    exit(1);
	}
    }

    if (do_listen && listen(sockfd, qlen) < 0) {
	perror("listen()");
	exit(1);
    }

    return sockfd;
}

int tobind(const char * addr)
{
    return tobindex(addr, DEFAULT_TCP_QLEN, NULL, 1);
}

int toconnect(const char *addr)
{
    return toconnectex(addr, -1);
}

int toconnectex(const char *addr, int timeout)
{
    return toconnect3(addr, timeout, 0);
}

int toconnect3(const char *addr, int timeout, int microseconds)
{
    int sock;
    
    assert(addr && *addr);

    if (!isdigit(addr[0]) && addr[0] != ':' && addr[0] != '*') {
	struct sockaddr_un serv_name;

	if ( (sock = socket(PF_UNIX, SOCK_STREAM, 0)) < 0 ) {
	    perror("socket");
	    return -1;
	}

	serv_name.sun_family = AF_UNIX;
	strlcpy(serv_name.sun_path, addr, sizeof(serv_name.sun_path));

	if (connect(sock, (struct sockaddr *)&serv_name, sizeof(serv_name)) < 0) {
	    close(sock);
	    return -1;
	}
    }
    else {
	char buf[64], *port;
	struct sockaddr_in serv_name;
	int was_block = 1;
	int timed = timeout > 0 || (timeout == 0 && microseconds > 0);

	if( (sock = socket(PF_INET, SOCK_STREAM, 0)) < 0 ){
	    perror("socket");
	    return -1;
	}

	if (timed)
	{
	    // set to non-block to allow timeout
	    int oflags = fcntl(sock, F_GETFL, NULL);
	    was_block = !(oflags & O_NONBLOCK);
	    fcntl(sock, F_SETFL, oflags | O_NONBLOCK);
	}

	strlcpy(buf, addr, sizeof(buf));
	if ( (port = strchr(buf, ':')) != NULL)
	    *port++ = '\0';

	assert(port && atoi(port) != 0);

	if (!buf[0] || buf[0] == '*')
	    serv_name.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	else
	    serv_name.sin_addr.s_addr = inet_addr(buf);

	serv_name.sin_port = htons(atoi(port));
	serv_name.sin_family = AF_INET;

	while (connect(sock, (struct sockaddr*)&serv_name, sizeof(serv_name)) < 0)
	{
	    if (errno == EINPROGRESS) 
	    {
		struct timeval tv = {0}; 
		fd_set myset; 

		assert(timed);
		// set timeout here
		tv.tv_sec = timeout;
		tv.tv_usec = microseconds;
		FD_ZERO(&myset); 
		FD_SET(sock, &myset); 

		if (select(sock+1, NULL, &myset, NULL, &tv) > 0)
		{
                    int result = 1;
                    socklen_t szresult = sizeof(result);
                    // szresult = 0: success, otherwise: failure.
                    getsockopt(sock, SOL_SOCKET, SO_ERROR, &result, &szresult);
                    if (result == 0)
                        break;
		}
	    }

	    // various failure
	    close(sock);
	    return -1;
	}

	if (timed)
	{
	    // restore flags
	    int oflags = fcntl(sock, F_GETFL, NULL);
	    if (was_block)
		oflags &= ~O_NONBLOCK;
	    else
		oflags |= O_NONBLOCK;
	    fcntl(sock, F_SETFL, oflags);
	}
    }

    return sock;
}

int is_to_readwrite_again(int s)
{
    if (s >= 0)
	return 0;
    if (errno == EINTR)
	return 1;
    if (errno == EAGAIN)
    {
#ifdef DEFAULT_IOWRITE_EAGAIN_WAIT_MICROSECOND
	// you can usleep here, to reduce system overhead
	usleep(DEFAULT_IOWRITE_EAGAIN_WAIT_MICROSECOND);
#endif
	return 1;
    }

    // all other case, failure.
    return 0;
}

/**
 * same as read(2), but read until exactly size len 
 */
int toread(int fd, void *buf, int len)
{
    int s = 0, t = 0;
    for(; len > 0 ;) {
	if ((s = read(fd, buf, len)) <= 0) {
	    if (is_to_readwrite_again(s))
		continue;
	    // XXX we define toread/towrite as '-1 for EOF and error'.
	    return -1; // s;
	} else {
	    buf += s;
	    len -= s;
            t += s;
	}
    }
    return t;
}

/**
 * same as write(2), but write until exactly size len 
 */
int towrite(int fd, const void *buf, int len)
{
    int s = 0, t = 0;
    for(; len > 0 ;) {
	if ((s = write(fd, buf, len)) <= 0) {
	    if (is_to_readwrite_again(s))
		continue;
	    // XXX we define toread/towrite as '-1 for EOF and error'.
	    return -1; // s;
	} else {
	    buf += s;
	    len -= s;
            t += s;
	}
    }
    return t;
}

/**
 * same as recv(2), but recv until exactly size len 
 */
int torecv(int fd, void *buf, int len, int flag)
{
    int s = 0, t = 0;
    for(; len > 0 ;) {
	if((s = recv(fd, buf, len, flag)) <= 0) {
	    if (is_to_readwrite_again(s))
		continue;
	    // XXX we define toread/towrite as '-1 for EOF and error'.
	    return -1; // s;
	} else {
	    buf += s;
	    len -= s;
            t += s;
	}
    }
    return t;
}

/**
 * similiar to send(2), but send until exactly size len 
 */
int tosend(int fd, const void *buf, int len, int flag)
{
    int s = 0, t = 0;
    for(; len > 0 ;) {
	if((s = send(fd, buf, len, flag)) <= 0){
	    if (is_to_readwrite_again(s))
		continue;
	    // XXX we define toread/towrite as '-1 for EOF and error'.
	    return -1; // s;
	} else {
	    buf += s;
	    len -= s;
            t += s;
	}
    }
    return t;
}

/**
 * fd sharing by piaip
 */

// return: -1 if error, otherwise success.
int send_remote_fd(int tunnel, int fd)
{
    struct msghdr   msg = {0};
    struct iovec    vec = {0};
    struct cmsghdr *cmsg;
    char   ccmsg [CMSG_SPACE(sizeof(fd))];
    char   dummy = 0;
    int    rv;

    vec.iov_base       = &dummy;	// must send/receive at least one byte
    vec.iov_len	       = 1;
    msg.msg_iov        = &vec;
    msg.msg_iovlen     = 1;
    msg.msg_control    = ccmsg;
    msg.msg_controllen = sizeof(ccmsg);

    cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type  = SCM_RIGHTS;
    cmsg->cmsg_len   = CMSG_LEN(sizeof(fd));
    *(int*)CMSG_DATA(cmsg) = fd;

    // adjust msg again
    msg.msg_controllen = cmsg->cmsg_len;
    msg.msg_flags      = 0;

    do {
	// ignore EINTR
	rv = sendmsg(tunnel, &msg, 0);
    } while (rv == -1 && errno == EINTR);

    if (rv == -1) {
	perror("sendmsg");
	return rv;
    }

    return 0;
}

// return: remote fd (-1 if error)
int recv_remote_fd(int tunnel, const char *tunnel_path)
{
    struct msghdr msg;
    struct iovec iov;
    char dummy;
    int rv;
    int connfd = -1;
    char ccmsg[CMSG_SPACE(sizeof(connfd))];
    struct cmsghdr *cmsg;
    struct sockaddr_un sun = {0};

    iov.iov_base    = &dummy;
    iov.iov_len	    = 1;
    msg.msg_iov	    = &iov;
    msg.msg_iovlen  = 1;
    msg.msg_control = ccmsg;
    msg.msg_controllen = sizeof(ccmsg);
    // XXX assigning msg_name here again is stupid,
    // but seems like Linux need it (while FreeBSD does not...)
    msg.msg_name    = &sun;
    msg.msg_namelen = sizeof(sun);
    sun.sun_family  = AF_UNIX;
    strlcpy(sun.sun_path, tunnel_path, sizeof(sun.sun_path));

    do {
	// ignore EINTR
	rv = recvmsg(tunnel, &msg, 0);
    } while (rv == -1 && errno == EINTR);

    if (rv == -1) {
	perror("recvmsg");
	return -1;
    }

    cmsg = CMSG_FIRSTHDR(&msg);
    if (!cmsg) {
	// kernel indicates there's no ancillary data
	return -1;
    }

    assert(cmsg->cmsg_type == SCM_RIGHTS);
    if (cmsg->cmsg_type != SCM_RIGHTS)
    {
	// invalid message!?
	return -1;
    }

    return *(int*)CMSG_DATA(cmsg);
}
