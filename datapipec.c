/*
 * Datapipe - Create a listen socket to pipe connections to another
 * machine/port. 'localport' accepts connections on the machine running    
 * datapipe, which will connect to 'remoteport' on 'remotehost'.
 * It will fork itself into the background on non-Windows machines.
 *
 * This implementation of the traditional "datapipe" does not depend on
 * forking to handle multiple simultaneous clients, and instead is able
 * to do all processing from within a single process, making it ideal
 * for low-memory environments.  The elimination of the fork also
 * allows it to be used in environments without fork, such as Win32.
 *
 * This implementation also differs from most others in that it allows
 * the specific IP address of the interface to listen on to be specified.
 * This is useful for machines that have multiple IP addresses.  The
 * specified listening address will also be used for making the outgoing
 * connections on.
 *
 * Run as:
 *   datapipe localhost localport remoteport remotehost
 *
 * Written by <admin@mrpro.me>
 * Based on datapipe written by Jeff Lawson <jlawson@bovine.net>
 * inspired by code originally by Todd Vierling, 1995.
 */

const char ident[] = "$Id: datapipec.c,v 1.2 2006/10/09 09:04:22 cadm Exp $";

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <assert.h>
#include <stdarg.h>
#include <ctype.h>
#if defined(__WIN32__) || defined(WIN32) || defined(_WIN32)
  #define WIN32_LEAN_AND_MEAN
  #include <winsock.h>
  #define bzero(p, l) memset(p, 0, l)
  #define bcopy(s, t, l) memmove(t, s, l)
#else
  #include <sys/time.h>
  #include <sys/types.h>
  #include <sys/socket.h>
  #include <sys/wait.h>
  #include <fcntl.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  #include <netdb.h>
  #include <strings.h>
  #include <poll.h>
  #include <syslog.h>
  #define recv(x,y,z,a) read(x,y,z)
  #define send(x,y,z,a) write(x,y,z)
  #define closesocket(s) close(s)
  typedef int SOCKET;
#endif

#include "base64.h"

#undef SMART_AUTH

#ifndef INADDR_NONE
#define INADDR_NONE 0xffffffff
#endif

#define MAXCLIENTS 4000
#define MAXFDS ((MAXCLIENTS)*2)
#define CLIENTSPERCHUNK 512
#define IDLETIMEOUT 90 /* seconds */

#define MAGIC_0 "\x71\x06\x00"
#define MAGIC_0_LEN (sizeof(MAGIC_0)-1)
#define MAGIC_1 "\x71"
#define MAGIC_1_LEN (sizeof(MAGIC_1)-1)
#define MAGIC_LEN (MAGIC_0_LEN+4+2+MAGIC_1_LEN)

#define BUFSIZE 128

struct iobuf_t {
  char buff[BUFSIZE];
  char *data; /* pointer to the first occupied byte in the buff */
  char *free; /* pointer to the first free byte */
};

struct client_t {
  struct client_t *next; /* next in a list */
  enum state_t {
    S_CONNECT, /* connection in progress */
    S_INIT,    /* Just allocated */
    S_AUTH_1,  /* First phase of authentication (407 code received)*/
    S_SKIP,    /* skip rest of stream */
    S_MAGIC,   /* magic has been read from client */
    S_RUN      /* transparent transmission */
  } state;
  int inuse;
  size_t h_length;
  struct sock_t {
    SOCKET s;
    struct iobuf_t buf;   /* input buffer for the socket */
    struct iobuf_t *obuf; /* pointer to output buffer (which, in fact, is an input buffer of
			   the sibling socket */
  } csock, /* <c>lient socket */ 
    osock; /* <o>utgoing socket */
  time_t activity; /* moment of time, when the connection must be terminated */
  //  in_port_t sin_port;
  //  struct in_addr sin_addr;
};

struct timer_t {
  struct timer_t *next;
  time_t diff;
  struct client_t *cli;
};

/* prototypes */

void debug(int level, char *fmt, ...);

int iobuf_wants_read(const struct iobuf_t *buf);
int iobuf_read(struct iobuf_t *buf, SOCKET fd);
int iobuf_wants_write(const struct iobuf_t *buf);
int iobuf_write(struct iobuf_t *buf, SOCKET fd);
void iobuf_init(struct iobuf_t *buf);
size_t iobuf_free_len(const struct iobuf_t *bp);
size_t iobuf_data_len(const struct iobuf_t *bp);
void iobuf_append(struct iobuf_t *bp, size_t len);
void iobuf_remove(struct iobuf_t *bp, size_t len);
int iobuf_skip_crlf(struct iobuf_t *bp);
char *iobuf_free_addr(struct iobuf_t *bp);
char *iobuf_data_addr(struct iobuf_t *bp);
size_t iobuf_push(struct iobuf_t *bp, const char *c, size_t len);
size_t iobuf_pushz(struct iobuf_t *bp, const char *c);

struct sock_t *client_find_sock(int fd, struct client_t **cli_p);
struct client_t *client_allocate(void);
void client_append_to_active_list(struct client_t *cp);
void client_init(struct client_t *cp);
int client_list_length(const struct client_t *cc);
void client_skip_junk(struct client_t *cc);
void client_interp(struct client_t *cc);
void client_connected(struct client_t *cc);
void client_parse_header(struct client_t *cc);

struct sockaddr_in *get_addr(struct sockaddr_in *p_addr);
struct sockaddr_in *parse_addr(const char *addr, const char *port);
void dump_poll_list(int level, struct pollfd *pfd, size_t n_pfd);

int str_begins_with(const char *str, size_t size, const char *patt);

/* GLOBALs and STATICs */

int demonize_flag = 0;
int debug_level = 0;
time_t socket_timeout = IDLETIMEOUT; /* secs */

static char *connect_string;
static char *credentials = 0;

struct client_t *active_clients = 0; /* the list of active clients */
struct client_t *free_clients = 0; /* the list of free clients */ 
time_t now; /* current time */

static void
prepare_sock(struct sock_t *sp, struct pollfd *fds_p)
{
  fds_p->events = 0;
  fds_p->revents = 0;
  debug(2, "**** main loop: socket == %d ****\n", sp->s);
  if(iobuf_wants_read(&sp->buf)){
    fds_p->fd = sp->s;
    fds_p->events |= POLLIN;
  }
  if(iobuf_wants_write(sp->obuf)){
    fds_p->fd = sp->s;
    fds_p->events |= POLLOUT;
  }
}
 
void
set_credentials(const char *str)
{
  int n;
  n = base64_encode(str, strlen(str), &credentials);
  assert(n != 0);
  //  debug(1, "set_credentials(%s) sets %s\n", str, credentials);
}

int 
main(int argc, char *argv[])
{ 
  SOCKET lsock;
  struct sockaddr_in laddr;
  struct sockaddr_in *p_addr;
  int i;
  struct client_t clients[MAXCLIENTS];
  int opt;

  openlog("datapipe", LOG_PID|LOG_PERROR, LOG_DAEMON);
  if(getenv("DATAPIPE_DEBUG"))
    debug_level = atoi(getenv("DATAPIPE_DEBUG"));
  if(getenv("DATAPIPE_AUTH"))
    set_credentials(getenv("DATAPIPE_AUTH"));

#if defined(__WIN32__) || defined(WIN32) || defined(_WIN32)
  /* Winsock needs additional startup activities */
  WSADATA wsadata;
  WSAStartup(MAKEWORD(1,1), &wsadata);
#endif

  /* check number of command line arguments */
  if (argc != 6) {
    fprintf(stderr,"Usage: %s localhost localport remotehost remoteport connect-string\n",argv[0]);
    fprintf(stderr, "ENVIRONMENT:\n");
    fprintf(stderr, "\tDATAPIPE_DEBUG to set debug_level\n");
    fprintf(stderr, "\tDATAPIPE_AUTH='user:password' to set login info\n");
    fprintf(stderr, "\nCVS Version: %s\nCompiled on: %s %s\n", ident, __DATE__, __TIME__);
    return 30;
  }

  if(!credentials){
    char *cp;
    char buf[80];

    if(fgets(buf, sizeof(buf), stdin)){
      cp = strchr(buf, '\n');
      if(cp) *cp = 0;
      set_credentials(buf);
    }else{
      fprintf(stderr, "Can't read auth info from stdin\n");
      exit(1);
    }
  }

  connect_string = strdup(argv[5]);

  /* reset all of the client structures */
  for (i = 0; i < MAXCLIENTS; i++)
    clients[i].inuse = 0;

  /* determine the listener address and port */
  bzero(&laddr, sizeof(struct sockaddr_in));
  laddr.sin_family = AF_INET;
  laddr.sin_port = htons((unsigned short) atol(argv[2]));
  laddr.sin_addr.s_addr = inet_addr(argv[1]);
  if (!laddr.sin_port) {
    fprintf(stderr, "invalid listener port\n");
    return 20;
  }
  if (laddr.sin_addr.s_addr == INADDR_NONE) {
    struct hostent *n;
    if ((n = gethostbyname(argv[1])) == NULL) {
      perror("gethostbyname");
      return 20;
    }    
    bcopy(n->h_addr, (char *) &laddr.sin_addr, n->h_length);
  }

  p_addr = parse_addr(argv[3], argv[4]);

  /* create the listener socket */
  if ((lsock = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
    perror("socket");
    return 20;
  }
  opt = 1;
  if(setsockopt(lsock, SOL_SOCKET, SO_REUSEADDR, (char *)&opt, sizeof(opt)) == -1){
    perror("setsockopt");
    return 20;
  }
  if (bind(lsock, (struct sockaddr *)&laddr, sizeof(laddr))) {
    perror("bind");
    return 20;
  }
  if (listen(lsock, 128)) {
    perror("listen");
    return 20;
  }

  /* change the port in the listener struct to zero, since we will
   * use it for binding to outgoing local sockets in the future. */
  laddr.sin_port = htons(0);

  /* fork off into the background. */
#if !defined(__WIN32__) && !defined(WIN32) && !defined(_WIN32)
  if(demonize_flag){
    if ((i = fork()) == -1) {
      perror("fork");
      return 20;
    }
    if (i > 0)
      return 0;
    setsid();
  }
#endif
  
  /* main polling loop. */
  now = time(NULL);
  while(1) {
    int rc;
    struct pollfd fds[MAXFDS], *fds_p;
    time_t tmo = socket_timeout; /* tmo is mesured in seconds */
    struct client_t *cc, *pc; /* current & previous elements in active list */

    pc = 0;
    fds_p = fds;
    /* First is the listening socket */
    fds_p->fd = lsock;
    fds_p->events = POLLIN;
    fds_p->revents = 0;
    fds_p++;
    /* Now let's add all the active sockets */
    for(cc = active_clients ; cc ; ){
      debug(1, "------- Clients in list: %d --------\n", client_list_length(cc));
      debug(1, "cc->activity %d now %d\n", cc->activity, now);
      if(cc->csock.s == -1 || cc->osock.s == -1 || cc->activity <= now){
	/* remove the current element from the chain 
	   return it to the free list;
	 */
	debug(2, "remove client from the list cc = (0x%0x)\n", (unsigned)cc);
	if(cc->csock.s != -1)
	  closesocket(cc->csock.s);
	if(cc->osock.s != -1)
	  closesocket(cc->osock.s);
	struct client_t *tmp = cc;
	if(pc){
	  pc->next = pc->next->next;
	}else{
	  active_clients = cc->next;
	}
	cc = cc->next;
	tmp->next = free_clients;
	free_clients = tmp;
      }else{
	struct sock_t *sp;
	if(tmo < cc->activity - now)
	  tmo = cc->activity - now;
	if(cc->state == S_CONNECT){
	  debug(2, "**** main loop: socket (connect) == %d ****\n", cc->osock.s);
	  fds_p->fd = cc->osock.s;
	  fds_p->events |= POLLOUT;
	  fds_p++;
	}else{
	  for(sp = &cc->csock ; sp - &cc->csock < 2 ; sp++){
	    prepare_sock(sp, fds_p); 
	    if(fds_p->events)
	      fds_p++;
	  }
	}
	pc = cc; /* pc is pointer to the previous client */
	cc = cc->next;
      }
    }
    debug(3, "just before poll(,%d,%d)...\n", fds_p - fds, tmo);
    dump_poll_list(3, fds, fds_p - fds);
    nfds_t fds_count = fds_p - fds;
    rc = poll(fds, fds_count, tmo * 1000);
    now = time(NULL);
    debug(3, "just after poll(,%d,10) == %d\n", fds_count, rc);
    if(rc < 0){
      syslog(LOG_ERR, "poll error. %m");
      break;
    }
    if(rc == 0){
      debug(10, "poll timeout\n");
      continue;
    }
    if (fds[0].revents != 0){
        rc--;
    }
    if(fds[0].revents & POLLIN){
      struct sockaddr_in s_addr;
      socklen_t addrlen = sizeof(s_addr);
      SOCKET csock = accept(lsock, (struct sockaddr *)&s_addr, &addrlen);
      if(csock != -1){
	debug(2, "request for incoming connection...\n");
	debug(2, "connected from: %s:%d\n", inet_ntoa(s_addr.sin_addr), ntohs(s_addr.sin_port));
	/* allocate new client structure. The structure is still on the free list */
	struct client_t *cc = client_allocate();
	if(cc){
	  SOCKET osock;

	  cc->csock.s = csock;
	  //	  cc->sin_port = s_addr.sin_port;
	  //	  cc->sin_addr = s_addr.sin_addr;
	  cc->state = S_CONNECT;

	  /* connect a socket to the outgoing host/port */
	  if ((osock = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
	    perror("socket");
	    closesocket(csock);
	  } else if (bind(osock, (struct sockaddr *)&laddr, sizeof(laddr))) {
	    perror("bind");
	    closesocket(csock);
	    closesocket(osock);
	  } else {
	    struct sockaddr_in *to_addr;
	    to_addr = get_addr(p_addr);

	    if(fcntl(csock, F_SETFL, O_NONBLOCK) == -1){
	      perror("fcntl(csock, F_SETFL, O_NONBLOCK)");
	    }
	    if(fcntl(osock, F_SETFL, O_NONBLOCK) == -1){
	      perror("fcntl(osock, F_SETFL, O_NONBLOCK)");
	    }
	    cc->osock.s = osock;
	    cc->csock.s = csock;
	    debug(2, "connecting to: %s:%d\n", inet_ntoa(to_addr->sin_addr), ntohs(to_addr->sin_port));
	    if(connect(osock, (struct sockaddr *)to_addr, sizeof(*to_addr))){
	      if(errno == EINPROGRESS){
		debug(2, "client built (delayed connection): (%d,%d)\n", csock, osock); 		
		client_append_to_active_list(cc);
	      }else{
		/* A fatal error encountered. The cc structure does not need to be freed
		   because it has not been appended to the active list and still is in
		   free list.
		*/
		perror("connect");
		closesocket(csock);
		closesocket(osock);
	      }
	    }else{
	      debug(2, "client built (instant connection): (%d,%d)\n", csock, osock); 
	      client_append_to_active_list(cc);
	      client_connected(cc);
	    }
	  }
	}else{
	  /* can't allocate cleint structure */
	  syslog(LOG_ERR, "Can't allocate client structure");
	  closesocket(csock);
	}
      }
    }
    for(fds_p = fds+1 ; rc && ((fds_p - fds) < fds_count) ; fds_p++){
      if(fds_p->revents & (POLLOUT|POLLIN)){
	struct client_t *cli;
	struct sock_t *sp;

	debug(3, "Event on fd = %d revent = 0x%x\n", fds_p->fd, fds_p->revents);

	sp = client_find_sock(fds_p->fd, &cli);
	assert(sp);
	if(cli->state == S_CONNECT){
	  /* check if connection was established */
	  struct sockaddr_in name;
	  socklen_t namelen;
	  int err;
	  namelen = sizeof(err);
	  err = 0;
	  if(getsockopt(cli->osock.s, SOL_SOCKET, SO_ERROR, &err, &namelen)){
	    syslog(LOG_ERR, "getsockopt failed: %m");
	    closesocket(cli->osock.s);
	    cli->osock.s = -1;
	  }else{
	    errno = err;
	    namelen = sizeof(name);
	    if(errno == 0 && getpeername(cli->osock.s, (struct sockaddr *)&name, &namelen) == 0){
	      debug(2, "Connection established: fd=%d %s\n", fds_p->fd, inet_ntoa(name.sin_addr));
	      client_connected(cli);
	    }else{
	      syslog(LOG_ERR, "connection failed: %m");
	      closesocket(cli->osock.s);
	      cli->osock.s = -1;
	    }
	  }
	}else{
	  int closeindeed = 0;

	  if(fds_p->revents & POLLOUT){
	    if(iobuf_write(sp->obuf, fds_p->fd) <= 0)
	    closeindeed = 1;
	  }
	  if(fds_p->revents & POLLIN){
	    if(iobuf_read(&sp->buf, fds_p->fd) <= 0)
	      closeindeed = 1;
	    else{
	      /* read was successful */
	      client_interp(cli);
	    }
	  }
	  cli->activity = now + socket_timeout;
	  if(closeindeed){
	    closesocket(sp->s);
	    sp->s = -1;
	  }
	}
      }
      if (fds_p->revents != 0){
          rc--;
      }
    } /* for(fds_p = fds+1... */
  } /* while (1) */
  return 0;
}

int 
iobuf_wants_read(const struct iobuf_t *bp)
{
  debug(5, "iobuf_wants_read(0x%0x)... \n", (unsigned)bp);
  return iobuf_free_len(bp) > 0;
}

int
iobuf_wants_write(const struct iobuf_t *bp)
{
  debug(5, "iobuf_wants_write(0x%0x)... \n", (unsigned)bp);
  return iobuf_data_len(bp) > 0;
}

struct client_t *
client_allocate(void)
{
  if(!free_clients){
    struct client_t *p = calloc(sizeof(struct client_t), CLIENTSPERCHUNK);
    if(!p){
      syslog(LOG_ERR, "Can't allocate memory for client structures");
      return 0;
    }
    syslog(LOG_DEBUG, "chunk allocated (%d elements)", CLIENTSPERCHUNK); 
    int n = CLIENTSPERCHUNK - 1;
    free_clients = p;
    while(n--){
      p->next = p+1;
      p++;
    }
  }
  return free_clients;
}

void
client_append_to_active_list(struct client_t *cc)
{
  assert(cc == free_clients);
  free_clients = cc->next; /* element has just been excluded from the free list */
  cc->next = active_clients ;
  active_clients = cc;
  cc->activity = now + socket_timeout;
  client_init(cc);
}

/*
 * TODO: implement more sophisticated search algorithm
 */
struct sock_t *
client_find_sock(int fd, struct client_t **cli_p)
{
  struct sock_t *r = 0;
  struct client_t *cp;

  debug(5, "client_find_sock(%d)...\n", fd);
  *cli_p = 0;
  for(cp = active_clients ; cp ; cp = cp->next){
    if(cp->csock.s == fd){
      r = &cp->csock;
      *cli_p = cp;
      break;
    }
    if(cp->osock.s == fd){
      r = &cp->osock;
      *cli_p = cp;
      break;
    }
  }
  return r;
}

int
client_list_length(const struct client_t *cc)
{
  const int toomany = 10000;
  int cnt;

  for(cnt = 0 ; cnt < toomany && cc ; cnt++) {
    cc = cc->next;
  }
  return cnt == toomany ? -1 : cnt;
}

int
iobuf_write(struct iobuf_t *bp, int fd)
{
  ssize_t rc;
  size_t len = iobuf_data_len(bp);

  assert(len);
  debug(5, "iobuf_write: before write(%d, , %u)...\n", fd, len);
  rc = write(fd, iobuf_data_addr(bp), len);
  if(rc > 0){
    iobuf_remove(bp, rc);
  }
  debug(5, "iobuf_write(0x%0x, %d) == %d\n", (unsigned)bp, fd, rc);
  return rc;
}

int
iobuf_read(struct iobuf_t *bp, int fd)
{
  ssize_t rc;
  size_t len = iobuf_free_len(bp);

  if(len){
    rc = read(fd, iobuf_free_addr(bp), len);
    if(rc > 0){
      iobuf_append(bp, rc);
    }
  }
  debug(5, "iobuf_read(0x%0x, %d) == %d\n", (unsigned)bp, fd, rc);
  return rc;
}

int
iobuf_skip_crlf(struct iobuf_t *bp)
{
  size_t l = iobuf_data_len(bp);
  enum { S0, S1, CRLF } state;
  char *cp = bp->data;

  state = S0;
  while(l && state != CRLF){
    switch(state){
    case S0:
      if(*cp == '\r')
	state = S1;
      break;
    case S1:
      if(*cp == '\n')
	state = CRLF;
      break;
    case CRLF:
      ;
    }
    l--;
    cp++;
  }
  iobuf_remove(bp, cp - bp->data);
  return state == CRLF;
}

char *
iobuf_data_addr(struct iobuf_t *bp)
{
  if(bp->data == bp->buff + BUFSIZE)
    bp->data = bp->buff;
  return bp->data;
}

char *
iobuf_free_addr(struct iobuf_t *bp)
{
  if(bp->free == bp->buff + BUFSIZE)
    bp->free = bp->buff;
  return bp->free;
}

size_t
iobuf_pushz(struct iobuf_t *bp, const char *str)
{
  return iobuf_push(bp, str, strlen(str));
}

size_t
iobuf_push(struct iobuf_t *bp, const char *c, size_t len)
{
  size_t l = iobuf_free_len(bp);
  l = len > l ? l : len;
  memcpy(iobuf_free_addr(bp), c, l);
  iobuf_append(bp, l);

  return l;
}

size_t
iobuf_free_len(const struct iobuf_t *bp)
{
  size_t len;
  if(bp->free < bp->data)
    len = bp->data - bp->free;
  else{
    if(bp->free == bp->buff + BUFSIZE)
      len = bp->data - bp->buff;
    else
      len = bp->buff + BUFSIZE - bp->free;
  }
  debug(5, "iobuf_free_len(0x%0x) returns %d\n", (unsigned)bp, len);
  return len;
}

size_t
iobuf_data_len(const struct iobuf_t *bp)
{
  size_t len;
  if(bp->data < bp->free)
    len = bp->free - bp->data;
  else{
    if(bp->data == bp->data + BUFSIZE)
      len = bp->free - bp->buff;
    else
      len = bp->buff + BUFSIZE - bp->data;
  }
  debug(5, "iobuf_data_len(0x%0x) returns %d\n", (unsigned)bp, len);
  return len;
}

/*
 * add new data into buffer
 */
void
iobuf_append(struct iobuf_t *bp, size_t len)
{
  debug(5, "iobuf_append(0x%0x, %u)\n", (unsigned)bp, len);
  bp->free += len;
  if(bp->data == bp->buff + BUFSIZE)
    bp->data = bp->buff;
  assert(bp->free <= bp->buff + BUFSIZE);
}

/*
 * free some space in the buffer
 */
void
iobuf_remove(struct iobuf_t *bp, size_t len)
{
  bp->data += len;
  assert(bp->data <= bp->buff + BUFSIZE);
  if(bp->data == bp->free){
    bp->free = bp->buff;
    bp->data = bp->buff+BUFSIZE;
  }
}

void
client_init(struct client_t *cp)
{
  cp->csock.obuf = &cp->osock.buf;
  iobuf_init(&cp->csock.buf);
  cp->osock.obuf = &cp->csock.buf;
  iobuf_init(&cp->osock.buf);
}

/*
 *   +---+----------+---+----------------------------+
 *   i   i  . . .   i   i                            i
 *   +---+----------+---+----------------------------+
 *                    ^
 * Possible pointer configurations:
 *    1) buff <= data < free <= buff+BUFSIZE
 *    2) data == free - the buffer is FULL
 *    3) buff <= free < data <= 
 *
 *
 */

void
iobuf_init(struct iobuf_t *bp)
{
  bp->free = bp->buff;
  bp->data = bp->buff+BUFSIZE;
}

/*
 *
 *
 */

void 
debug(int level, char *fmt, ...)
{
  va_list ap;

  if(level <= debug_level){
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
  }

}

/*
 *
 */

#if 0
struct  hostent {
  char    *h_name;        /* official name of host */
  char    **h_aliases;    /* alias list */
  int     h_addrtype;     /* host address type */
  int     h_length;       /* length of address */
  char    **h_addr_list;  /* list of addresses from name server */
};
#endif // 0

struct sockaddr_in *
parse_addr(const char *addr, const char *port)
{
  int cnt;
  struct sockaddr_in *p_addr;
  uint16_t _port = htons((unsigned short) atol(port));

  /* determine the outgoing address and port */
  if(!_port){
    fprintf(stderr, "invalid target port\n");
    exit(EXIT_FAILURE);
  }

  if(inet_addr(addr) != INADDR_NONE){
    /* the address is in the dotted form */ 
    p_addr = calloc(2, sizeof(*p_addr));
    assert(p_addr);
    p_addr[0].sin_family = AF_INET;
    p_addr[0].sin_port = _port;
    p_addr[0].sin_addr.s_addr = inet_addr(addr);
    p_addr[1].sin_addr.s_addr = INADDR_NONE;
  }else{
    char **cp;
    struct hostent *n;

    if ((n = gethostbyname(addr)) == NULL) {
      perror("gethostbyname");
      exit(EXIT_FAILURE);
    }
    
    for(cp = n->h_addr_list ; *cp ; cp++)
      ;
    cnt = cp - n->h_addr_list;
    p_addr = calloc(cnt+1, sizeof(*p_addr));
    for(cp = n->h_addr_list ; *cp ; cp++){
      memcpy((char *)&p_addr->sin_addr.s_addr, *cp, n->h_length);
      p_addr->sin_family = AF_INET;
      p_addr->sin_port = _port;
      p_addr++;
    }
    p_addr->sin_addr.s_addr = INADDR_NONE;
    p_addr -= cnt;
  }
  assert(p_addr);
  return p_addr;
}

/*
 *
 */
struct sockaddr_in *
get_addr(struct sockaddr_in *p_addr)
{
  static size_t cnt = 0;
  static struct sockaddr_in *curr_p;

  if(!cnt){
    curr_p = p_addr;
    while(curr_p->sin_addr.s_addr != INADDR_NONE){
      ++curr_p;
    }
    cnt = curr_p - p_addr;
  }
  if(curr_p == p_addr + cnt)
    curr_p = p_addr;
  return curr_p++;
}

/*
 *
 * Escape character is '^]'.
 * CONNECT mydomain.io:110 HTTP/1.1
 *
 * HTTP/1.0 200 Connection established
 *
 *
 * or
 *
 * HTTP/1.0 407 Proxy Authentication Required
 * Server: squid/2.5.STABLE7
 * Mime-Version: 1.0
 * Date: Mon, 28 Feb 2005 12:27:31 GMT
 * Content-Type: text/html
 * Content-Length: 1281
 * Expires: Mon, 28 Feb 2005 12:27:31 GMT
 * X-Squid-Error: ERR_CACHE_ACCESS_DENIED 0
 * Proxy-Authenticate: Basic realm="Squid proxy-caching web server"
 * X-Cache: MISS from fw.mydomain.org
 * Proxy-Connection: keep-alive
 *
 */
void
client_interp(struct client_t *cc) 
{
  size_t l;
  debug(1, "client_interp -- start\n");
  switch(cc->state){
  case S_CONNECT:
    break;
  case S_INIT:
    if((l = iobuf_data_len(&cc->osock.buf)) >= strlen("HTTP/1.0 xxx ")){
      if(strncmp(cc->osock.buf.data, "HTTP/1.", 7) == 0 
	 && strncmp(cc->osock.buf.data+8, " 200 ", 5) == 0){
	/* Well... We've got positive answer. Now we have to remove the entire 
	   header from the input buffer. The header is termitated by empty line:
	   ...\r\n\r\n
	*/
	char *cp = cc->osock.buf.data;
	int st = 0;
	while(l && cc->state == S_INIT){ 
	  switch(st){
	  case 0:
	    if(*cp == '\r')
	      st = 1;
	    l--; cp++;
	    break;
	  case 1:
	    if(*cp == '\n'){
	      st = 2;
	      l--; cp++;
	    }else
	      st = 0;
	    break;
	  case 2:
	    if(*cp == '\r'){
	      st = 3;
	      l--; cp++;
	    }else
	      st = 0;
	    break;
	  case 3:
	    if(*cp == '\n'){
	      l--; cp++;
	      iobuf_remove(&cc->osock.buf,  cp - cc->osock.buf.data);
	      cc->state = S_RUN;
	      debug(1, "client_interp: length(osock.buf.data) == %d\n", iobuf_data_len(&cc->osock.buf));
	    }
	  }
	} /* while() */
#if defined(SMART_AUTH)
      }else if(strncmp(cc->osock.buf.data, "HTTP/1.", 7) == 0
	       && strncmp(cc->osock.buf.data+8, " 407 ", 5) == 0){
	/* Proxy Authentification Required */
	if(iobuf_skip_crlf(&cc->osock.buf)){
	  cc->state = S_AUTH_1;
	  client_parse_header(cc);
	}
#endif /* 0 */
      }else{
	/* Terminate the connection */

	char txt[40];
	char *f, *t;

	f = cc->osock.buf.data;
	t = txt;
	while(*f && *f != '\r' && *f != '\n' && t - txt < sizeof(txt) - 1)
	  *t++ = *f++;
	*t++ = 0;
	syslog(LOG_ERR, "BAD proxy response: %s", txt);
	closesocket(cc->osock.s);
	cc->osock.s = -1;
	closesocket(cc->csock.s);
	cc->csock.s = -1;
      }
    }
    break;
  case S_AUTH_1:
    client_parse_header(cc);
    break;
  case S_SKIP:
    client_skip_junk(cc);
    break;
  case S_MAGIC:
    /* this state is obsoleted */
    break;
  case S_RUN:
    break;
  }
}

/* 
 * Proxy-Authorization: Basic eXlrOnl5aw==
 */

void
client_skip_junk(struct client_t *cc)
{
  size_t l = iobuf_data_len(&cc->osock.buf);
  if(cc->h_length <= l){
    iobuf_remove(&cc->osock.buf, cc->h_length);
    cc->h_length = 0;
    cc->state = S_INIT;
    /* send our credentials */
    iobuf_pushz(cc->osock.obuf, "CONNECT ");
    iobuf_pushz(cc->osock.obuf, connect_string);
#if SMART_AUTH
    iobuf_pushz(cc->osock.obuf, " HTTP/1.0\r\nConnection: keep-alive\r\n");
#endif /* SMART_AUTH */
    if(credentials){
      iobuf_pushz(cc->osock.obuf, "Proxy-Authorization: Basic ");
      iobuf_pushz(cc->osock.obuf, credentials);
      iobuf_pushz(cc->osock.obuf, "\r\n");
    }
    iobuf_pushz(cc->osock.obuf, "\r\n");
  }else{
    iobuf_remove(&cc->osock.buf, l);
    cc->h_length -= l;
  }
}

int
str_begins_with(const char *str, size_t len, const char *patt)
{
  size_t n = strlen(patt);

  return len >= n && strncmp(str, patt, n) == 0;
}

/*
 * We reach this place only if the proxy returned the 407 reply code.
 * The pointer is in the beginning of the current line of the response header
 * The main task is to find out what authentication scheme is requested and 
 * skip up to the end of the response
 *
 * Server: squid/2.5.STABLE7
 * Mime-Version: 1.0
 * Date: Mon, 28 Feb 2005 12:27:31 GMT
 * Content-Type: text/html
 * Content-Length: 1281
 * Expires: Mon, 28 Feb 2005 12:27:31 GMT
 * X-Squid-Error: ERR_CACHE_ACCESS_DENIED 0
 * Proxy-Authenticate: Basic realm="Squid proxy-caching web server"
 * X-Cache: MISS from fw.mydomain.org
 * Proxy-Connection: keep-alive
 */
void
client_parse_header(struct client_t *cc)
{
  size_t l;
  char *cp;

  cp = cc->osock.buf.data;
  l = iobuf_data_len(&cc->osock.buf);
  while(1){
    if(l >= 2 && cp[0] == '\r' && cp[1] == '\n'){
      /* The end of the header */
      iobuf_remove(&cc->osock.buf, 2);
      cc->state = S_SKIP;
      client_skip_junk(cc);
      break;
    }
    if(str_begins_with(cp, l, "Content-Length: ")){
      size_t n;
      cp += strlen("Content-Length: ");
      l -= strlen("Content-Length: ");
      n = 0;
      while(l--){
	if(isdigit(*cp)){
	  n *= 10;
	  n += *cp - '0';
	  cp++;
	}
      }
      if(!(l >= 2 && cp[0] == '\r' && cp[1] == '\n')){
	/* current line is not terminated with CRLF */
	break;
      }
      cc->h_length = n;
      cp += 2;
      l -= 2;
      iobuf_remove(&cc->osock.buf, cp - cc->osock.buf.data);
    }else if(str_begins_with(cp, l, "Proxy-Authenticate: ")){
      cp += strlen("Proxy-Authenticate: ");
      l -= strlen("Proxy-Authenticate: ");
      /* TODO: check for supported number of auth methods */
      /* for now */
      if(!iobuf_skip_crlf(&cc->osock.buf))
        break;
    }else{
      if(!iobuf_skip_crlf(&cc->osock.buf)){
	/* current line is not terminated with CRLF */
	break;
      }
    }
    cp = cc->osock.buf.data;
    l = iobuf_data_len(&cc->osock.buf);
  } /* while(... */
}



void
client_connected(struct client_t *cc)
{
  iobuf_pushz(cc->osock.obuf, "CONNECT ");
  iobuf_pushz(cc->osock.obuf, connect_string);
  iobuf_pushz(cc->osock.obuf, " HTTP/1.0\r\n");
#if SMART_AUTH
  iobuf_pushz(cc->osock.obuf, "Connection: keep-alive\r\n");
#endif /* SMART_AUTH */
  if(credentials){
    iobuf_pushz(cc->osock.obuf, "Proxy-Authorization: Basic ");
    iobuf_pushz(cc->osock.obuf, credentials);
    iobuf_pushz(cc->osock.obuf, "\r\n");
  }
  iobuf_pushz(cc->osock.obuf, "\r\n");
  cc->state = S_INIT;
}

void
dump_poll_list(int level, struct pollfd *pfd, size_t n_pfd)
{
  if(level <= debug_level){
    size_t i;
    for(i = 0 ; i < n_pfd ; i++){
      fprintf(stderr, "%d: ", pfd[i].fd);
      if(pfd[i].events & POLLIN)
	fprintf(stderr, "POLLIN|");
      if(pfd[i].events & POLLOUT)
        fprintf(stderr, "POLLOUT|");
      fprintf(stderr, " ");
    }
    fprintf(stderr, "\n");
  }
}
