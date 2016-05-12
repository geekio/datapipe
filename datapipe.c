/*
 * Datapipe - Create a listen socket to pipe connections to another
 * machine/port.
 *
 * Run as:
 *   datapipe [options] l_host l_port r_port r_host [src_addr]
 *
 * Written by <admin@mrpro.me>
 * Based on datapipe written by Jeff Lawson <jlawson@bovine.net>
 * inspired by code originally by Todd Vierling, 1995.
 */

const char ident[] = "$Id: datapipe.c,v 1.28 2007/09/25 11:47:25 cadm Exp $";

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <signal.h>
#include <assert.h>
#include <stdarg.h>
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

#ifndef INADDR_NONE
#define INADDR_NONE 0xffffffff
#endif

#define MAXDSTIP 256 /* max number of dst IP addresses */
#define MAXSONS n_forks
#define MAXCLIENTS (31 * 1024/(MAXSONS+1))
#define MAXFDS ((MAXCLIENTS)*2)
#if DYNAMIC_CLIENTS
#define CLIENTSPERCHUNK 512
#else
#define CLIENTSPERCHUNK MAXCLIENTS
#endif // DYNAMIC_CLIENTS
#define IDLETIMEOUT 90 /* seconds */
#define CONNTIMEOUT 15 /* seconds */

#define MAGIC_0 "\x71\x06\x00"
#define MAGIC_0_LEN (sizeof(MAGIC_0)-1)
#define MAGIC_1 "\x71"
#define MAGIC_1_LEN (sizeof(MAGIC_1)-1)
#define MAGIC_LEN (MAGIC_0_LEN+4+2+MAGIC_1_LEN)

#define BUFSIZE 2048

typedef uint64_t counter_t;

struct iobuf_t {
  char buff[BUFSIZE];
  char *data; /* pointer to the first occupied byte in the _buf */
  char *free; /* pointer to the first free byte */
};

struct client_t {
  struct client_t *next; /* next in a list */
  enum state_t {
    S_CONNECT, /* connection in progress */
    S_INIT,   /* Just allocated */
    S_MAGIC,  /* magic has been read from client */
    S_RUN     /* transparent transmission */
  } state;
  size_t fail_cnt;
  struct sock_t {
    SOCKET s;
    struct iobuf_t buf;   /* input buffer for the socket */
    struct iobuf_t *obuf; /* pointer to output buffer (which, in fact, is an input buffer of
			   the sibling socket */
  } csock, osock;
  time_t activity; /* moment of time, when the connection must be terminated */
  //  in_port_t sin_port;
  //  struct in_addr sin_addr;
};

struct timer_t {
  struct timer_t *next;
  time_t diff;
  struct client_t *cli;
};

struct client_t **socket_index = 0;  // calloc([MAXFDS+10],...);
int socket_index_size = 0;

/* prototypes */

#ifdef WITH_DEBUG
void _debug(int level, char *fmt, ...);
#define debug(x) _debug x
#else
#define debug(x)
#endif

static int iobuf_wants_read(const struct iobuf_t *buf);
int iobuf_read(struct iobuf_t *buf, SOCKET fd);
static int iobuf_wants_write(const struct iobuf_t *buf);
int iobuf_write(struct iobuf_t *buf, SOCKET fd);
void iobuf_init(struct iobuf_t *buf);
size_t iobuf_free_len(const struct iobuf_t *bp);
size_t iobuf_data_len(const struct iobuf_t *bp);
void iobuf_append(struct iobuf_t *bp, size_t len);
void iobuf_remove(struct iobuf_t *bp, size_t len);
char *iobuf_free_addr(struct iobuf_t *bp);
char *iobuf_data_addr(struct iobuf_t *bp);
size_t iobuf_push(struct iobuf_t *bp, const char *c, size_t len);

struct sock_t *client_find_sock(int fd, struct client_t **cli_p);
struct client_t *client_allocate(void);

void client_append_to_active_list(struct client_t *cp);
void client_connect_to_dst(struct client_t *cc, SOCKET csock);
void client_init(struct client_t *cc);
struct sockaddr_in *client_get_addr(struct client_t *cp, struct sockaddr_in *p_addr);
int client_list_length(const struct client_t *cc);
void client_interp(struct client_t *cc);
void client_connected(struct client_t *cc);

int register_socket(int fd, struct client_t *cc);

struct sockaddr_in *get_addr(struct sockaddr_in *p_addr);
struct sockaddr_in *parse_addr(const char *addr, const char *port);
void dump_poll_list(int level, struct pollfd *pfd, size_t n_pfd);
void on_term(int);

/* GLOBALs and STATICs */

int n_forks = 0;
int goon = 1;
int demonize_flag = 0;
int debug_level = 0;
time_t socket_timeout = IDLETIMEOUT; /* secs */
time_t connect_timeout = CONNTIMEOUT; /* secs */

struct client_t *active_clients = 0; /* the list of active clients */
struct client_t *free_clients = 0; /* the list of free clients */ 
time_t now; /* current time */

size_t p_addr_cnt = 0; /* Number of elements in p_addr array */
struct sockaddr_in *p_addr;
struct sockaddr_in *p_addr_curr;
struct sockaddr_in laddr;

struct {
  counter_t total_polls;
  counter_t total_events;
} statistics;

#ifndef DYNAMIC_CLIENTS
struct client_t *clients = 0; // = calloc(...[MAXCLIENTS];
#endif

//
// Almost inline functions
//

static int 
iobuf_wants_read(const struct iobuf_t *bp)
{
  debug((5, "iobuf_wants_read(0x%0x)... \n", (unsigned)bp));
  return iobuf_free_len(bp) > 0;
}

static int
iobuf_wants_write(const struct iobuf_t *bp)
{
  debug((5, "iobuf_wants_write(0x%0x)... \n", (unsigned)bp));
  return iobuf_data_len(bp) > 0;
}

static void
usage(const char *me)
{
  fprintf(stderr,"Usage: %s [-F n_forks][-L label][-t c_tmo][-T s_tmo] lhost lport rhost rport [ src_host ]\n", me);
  fprintf(stderr, "\trhost can be the list of comma separated addresses and/or names\n");
  fprintf(stderr, "Limits are:\n");
  fprintf(stderr, "\tMAXDSTIP          %5d\n", MAXDSTIP);
  fprintf(stderr, "\tBUFSIZE           %5d\n", BUFSIZE);
  fprintf(stderr, "\tMAXSONS           %5d\n", MAXSONS);
  fprintf(stderr, "\tMAXCLIENTS        %5d\n", MAXCLIENTS);
  fprintf(stderr, "\tMAXFDS            %5d\n", MAXFDS);
  fprintf(stderr, "\tCLIENTSPERCHUNK   %5d\n", CLIENTSPERCHUNK);
  fprintf(stderr, "\tsocket_index_size %5d\n", MAXFDS + 10);
  fprintf(stderr, "\tIDLETIMEOUT       %5d sec.\n", socket_timeout);
  fprintf(stderr, "\tCONNTIMEOUT       %5d sec.\n", connect_timeout);

  fprintf(stderr, "Compilation Options:\n");
#if SEND_CLIENT_IP
  fprintf(stderr, "\tSEND_CLIENT_IP option is ON\n");
#endif
#if DYNAMIC_CLIENTS
  fprintf(stderr, "\tDYNAMIC_CLIENTS option is ON\n");
#endif
#ifdef WITH_DEBUG
  fprintf(stderr, "You can use env DATAPIPE_DEBUG to set debug_level\n");
#endif // WITH_DEBUG
  fprintf(stderr, "\nCVS Version: %s\nCompiled on: %s %s\n", ident, __DATE__, __TIME__);
  exit(EXIT_FAILURE);
};

void
check_active_list(void)
{
  struct client_t *cp = active_clients;
  char sset[MAXFDS];
  memset(sset, 0, sizeof(sset));

  while(cp){
    if(cp->csock.s != -1){
      if(sset[cp->csock.s]){
	syslog(LOG_ERR, "socket csock.s = %d duplicated!", cp->csock.s);
	abort();
      }
      sset[cp->csock.s] = 1;
    }
    if(cp->osock.s != -1){
      if(sset[cp->osock.s]){
	syslog(LOG_ERR, "socket osock.s = %d duplicated!", cp->osock.s);
	abort();
      }
      sset[cp->osock.s] = 1;
    }
    cp = cp->next;
  }
}

int
main(int argc, char *argv[])
{ 
  SOCKET lsock;
  int i;
  int opt;
  int last_son;
  int ch;
  char *me = argv[0];
  char *from_addr;

  openlog("datapipe", LOG_PID|LOG_PERROR, LOG_DAEMON);
  if(getenv("DATAPIPE_DEBUG"))
    debug_level = atoi(getenv("DATAPIPE_DEBUG"));

  signal(SIGTERM, on_term);
  signal(SIGINT, on_term);
  signal(SIGPIPE, SIG_IGN);

#if defined(__WIN32__) || defined(WIN32) || defined(_WIN32)
  /* Winsock needs additional startup activities */
  WSADATA wsadata;
  WSAStartup(MAKEWORD(1,1), &wsadata);
#endif

  while((ch = getopt(argc, argv, "F:L:ht:T:")) != -1){
    switch(ch){
    case 'L':
      closelog();
      openlog(optarg, LOG_PID|LOG_PERROR, LOG_DAEMON);
      break;
    case 'F':
      n_forks = atoi(optarg);
      break;
    case 'T':
      socket_timeout = atoi(optarg);
      assert(socket_timeout > 0);
      break;
    case 't':
      connect_timeout = atoi(optarg);
      assert(connect_timeout > 0);
      break;
    case 'h':
    case '?':
      usage(me);
      break;
    }
  }

  argc -= optind;
  argv += optind;

  /* check number of command line arguments */
  switch(argc){
  case 4:
    from_addr = argv[0];
    break;
  case 5:
    from_addr = argv[4];
    break;
  default: 
    usage(me);
  }

  /* Allocate some data structures */

#ifndef DYNAMIC_CLIENTS
  clients = calloc(MAXCLIENTS, sizeof(struct client_t));
  assert(clients);
#endif // !DYNAMIC_CLIENTS

  socket_index_size = MAXFDS+10;
  socket_index = calloc(socket_index_size, sizeof(struct client_t *));
  assert(socket_index);

  /* determine the listener address and port */
  bzero(&laddr, sizeof(struct sockaddr_in));
  laddr.sin_family = AF_INET;
  laddr.sin_port = htons((unsigned short) atol(argv[1]));
  laddr.sin_addr.s_addr = inet_addr(argv[0]);
  if (!laddr.sin_port) {
    syslog(LOG_ERR, "invalid listener port");
    exit(EXIT_FAILURE);
  }
  if (laddr.sin_addr.s_addr == INADDR_NONE) {
    struct hostent *n;
    if ((n = gethostbyname(argv[0])) == NULL) {
      syslog(LOG_ERR, "gethostbyname failed. %m");
      exit(EXIT_FAILURE);
    }    
    bcopy(n->h_addr, (char *) &laddr.sin_addr, n->h_length);
  }

  p_addr_curr = p_addr = parse_addr(argv[2], argv[3]); /* as side effect it sets p_addr_cnt */

  /* create the listener socket */
  if ((lsock = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
    syslog(LOG_ERR, "socket failed %m");
    exit(EXIT_FAILURE);
  }
  opt = 1;
  if(setsockopt(lsock, SOL_SOCKET, SO_REUSEADDR, (char *)&opt, sizeof(opt)) == -1){
    syslog(LOG_ERR, "setsockopt failed %m");
    exit(EXIT_FAILURE);
  }
  if (bind(lsock, (struct sockaddr *)&laddr, sizeof(laddr))) {
    syslog(LOG_ERR, "bind failed %m");
    exit(EXIT_FAILURE);
  }
  if(fcntl(lsock, F_SETFL, O_NONBLOCK) == -1){
    syslog(LOG_ERR, "fcntl(csock, F_SETFL, O_NONBLOCK) failed %m");
    exit(EXIT_FAILURE);
  }
  if (listen(lsock, 128)) {
    syslog(LOG_ERR, "listen failed %m");
    exit(EXIT_FAILURE);
  }

  for(i = 0 ; i < MAXSONS ; i++){
    last_son = fork();
    if(!last_son)
      break;
  }

  syslog(LOG_INFO, "started");

  /* change the port in the listener struct to zero, since we will
   * use it for binding to outgoing local sockets in the future. 
   * Replace binding address with outgoing address from command line.
   */
  laddr.sin_port = htons(0);
  laddr.sin_addr.s_addr = inet_addr(from_addr);

  /* fork off into the background. */
#if !defined(__WIN32__) && !defined(WIN32) && !defined(_WIN32)
  if(demonize_flag){
    if ((i = fork()) == -1) {
      syslog(LOG_ERR, "fork failed %m");
      exit(EXIT_FAILURE);
    }
    if (i > 0)
      return 0;
    setsid();
  }
#endif
  
  /* main polling loop. */
  now = time(NULL);
  while(goon) {
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
      debug((2, "------- Clients in list: %d --------\n", client_list_length(cc)));
      debug((2, "cc->(csock,osock,fail_cnt,activity)=(%d,%d,%d,%d), now = %d\n", cc->csock.s, cc->osock.s, cc->fail_cnt, cc->activity, now));
      if(cc->csock.s == -1 || cc->osock.s == -1 || cc->activity <= now){
	if(cc->csock.s != -1 && cc->state == S_CONNECT && ++cc->fail_cnt < p_addr_cnt){
	  /* Initialize the connection to the server. Do not move the pointer down the list.
	     The pointer will be moved at the next loop turn-around
	  */
	  closesocket(cc->osock.s);
	  client_connect_to_dst(cc, cc->csock.s);
	  debug((2, "**** main loop: socket (RE-connect) == %d ****\n", cc->osock.s));
	}else{
	  debug((2, "remove client from the list cc = (0x%0x)\n", (unsigned)cc));
	  if(cc->csock.s != -1)
	    closesocket(cc->csock.s);
	  if(cc->osock.s != -1)
	    closesocket(cc->osock.s);

	  /* remove the current element from the chain 
	     return it to the free list;
	  */
	  struct client_t *tmp = cc;
	  if(pc){
	    pc->next = pc->next->next;
	  }else{
	    active_clients = cc->next;
	  }
	  cc = cc->next;
	  tmp->next = free_clients;
	  free_clients = tmp;
	}
      }else{
	struct sock_t *sp;
	if(tmo > cc->activity - now)
	  tmo = cc->activity - now;
	switch(cc->state){
	case S_CONNECT:
	  debug((2, "**** main loop: socket (connect) == %d ****\n", cc->osock.s));
	  fds_p->fd = cc->osock.s;
	  fds_p->events |= POLLOUT;
	  fds_p++;
	  break;
#if SEND_CLIENT_IP
	case S_INIT:
	  for(sp = &cc->csock ; sp - &cc->csock < 2 ; sp++){
	    fds_p->events = 0;
	    fds_p->revents = 0;
	    if(iobuf_wants_read(&sp->buf)){
	      fds_p->fd = sp->s;
	      fds_p->events |= POLLIN;
	    }
	    if(fds_p->events){
	      fds_p++;
	    }
	  }
	  break;
#endif // SEND_CLIENT_IP
	default:
	  for(sp = &cc->csock ; sp - &cc->csock < 2 ; sp++){
	    fds_p->events = 0;
	    fds_p->revents = 0;
	    debug((2, "**** main loop: socket == %d ****\n", sp->s));
	    if(iobuf_wants_read(&sp->buf)){
	      fds_p->fd = sp->s;
	      fds_p->events |= POLLIN;
	    }
	    if(iobuf_wants_write(sp->obuf)){
	      fds_p->fd = sp->s;
	      fds_p->events |= POLLOUT;
	    }
	    if(fds_p->events){
	      fds_p++;
	    }
	  }
	}
	pc = cc; /* pc is pointer to the previous client */
	cc = cc->next;
      }
    }
    debug((3, "just before poll(,%d,%d)...\n", fds_p - fds, tmo));
#ifdef WITH_DEBUG
    dump_poll_list(3, fds, fds_p - fds);
#endif // WITH_DEBUG
    statistics.total_polls++;

    /******* POLL *******/
    nfds_t fds_count = fds_p - fds;
    rc = poll(fds, fds_count, tmo * 1000);
    now = time(NULL);
    debug((3, "just after poll(,%d,%d) == %d\n", fds_count, tmo, rc));
    if(rc < 0){
      syslog(LOG_ERR, "poll error. %m");
      break;
    }
    if(rc == 0){
      debug((10, "poll timeout\n"));
      continue;
    }
    if(fds[0].revents != 0){
        rc--;
    }
    if(fds[0].revents & POLLIN){
      statistics.total_events++;

      struct sockaddr_in s_addr;
      socklen_t addrlen = sizeof(s_addr);
      while(1){
	// Try to accept as many connections as possible
	SOCKET csock = accept(lsock, (struct sockaddr *)&s_addr, &addrlen);
	if(csock == -1)
	  break;
	debug((2, "request for incoming connection...\n"));
	debug((2, "connected from: %s:%d\n", inet_ntoa(s_addr.sin_addr), ntohs(s_addr.sin_port)));
	/* allocate new client structure. The structure is still on the free list */
	struct client_t *cc = client_allocate();
	if(cc){
	  client_init(cc);
	  client_connect_to_dst(cc, csock);
	}else{
	  /* can't allocate client structure */
	  syslog(LOG_ERR, "Can't allocate client structure");
	  closesocket(csock);
	}
      } // while(1)
    } // if(fds[0].revents & POLLIN)...
    for(fds_p = fds+1 ; rc && ((fds_p - fds) < fds_count) ; fds_p++){
      if(fds_p->revents & (POLLOUT|POLLIN)){
	struct client_t *cli;
	struct sock_t *sp;

	debug((3, "Event on fd = %d revent = 0x%x\n", fds_p->fd, fds_p->revents));

	sp = client_find_sock(fds_p->fd, &cli);
	assert(sp);
	if(cli->state == S_CONNECT){
	  statistics.total_events++;
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
	      debug((2, "Connection established: fd=%d %s:%d\n", fds_p->fd, inet_ntoa(name.sin_addr), ntohs(name.sin_port)));
	      client_connected(cli);
	    }else{
	      syslog(LOG_ERR, "connection failed: %m");
	      closesocket(cli->osock.s);
	      cli->osock.s = -1;
	    }
	  }
	}else{ /* cli->state != S_CONNECT */
	  int closeindeed = 0;

	  if(fds_p->revents & POLLOUT){
	    statistics.total_events++;
	    if(iobuf_write(sp->obuf, fds_p->fd) <= 0)
	    closeindeed = 1;
	  }
	  if(fds_p->revents & POLLIN){
	    statistics.total_events++;
	    if(iobuf_read(&sp->buf, fds_p->fd) <= 0)
	      closeindeed = 1;
#if SEND_CLIENT_IP
	    else{
	      /* read was successful */
	      client_interp(cli);
	    }
#endif /* SEND_CLIENT_IP */
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
    //    check_active_list(); // DEBUG
  } /* while (goon) */

  if(last_son){
    while(wait(0) > 0)
      ;
  }

  syslog(LOG_INFO, "statistics.total_polls = %qd, statistics.total_events = %qd",
	  statistics.total_polls, statistics.total_events);

  return 0; /* Exit. Allmost impossible */
}

struct client_t *
client_allocate(void)
{
  if(!free_clients){
#if DYNAMIC_CLIENTS
    struct client_t *p = client_list_length(active_clients) + CLIENTSPERCHUNK > MAXCLIENTS 
      ? 0 :calloc(sizeof(struct client_t), CLIENTSPERCHUNK);
#else
    static int only_once = 0;
    struct client_t *p = 0;
    if(!only_once){
      only_once = 1;
      p = clients;
    }
#endif // DYNAMIC_CLIENTS
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

//
// move first element from free-clients list to the end of active-clients list
//

void
client_append_to_active_list(struct client_t *cc)
{
  if(cc == free_clients){
    /* The client has been just allocated */
    free_clients = cc->next; /* element has just been excluded from the free list */
    cc->next = active_clients ;
    active_clients = cc;
  }
  cc->activity = now + connect_timeout;

  cc->csock.obuf = &cc->osock.buf;
  iobuf_init(&cc->csock.buf);
  cc->osock.obuf = &cc->csock.buf;
  iobuf_init(&cc->osock.buf);
}

/*
 * TODO: implement more sophisticated search algorithm
 */
struct sock_t *
client_find_sock(int fd, struct client_t **cli_p)
{
  struct sock_t *r = 0;
  struct client_t *cp;

  debug((5, "client_find_sock(%d)...\n", fd));
  *cli_p = 0;
#if 0
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
#else
  assert(0 <= fd && fd < socket_index_size);
  cp = socket_index[fd];
  if(cp->csock.s == fd){
    r = &cp->csock;
    *cli_p = cp;
  }else if(cp->osock.s == fd){
    r = &cp->osock;
    *cli_p = cp;
  }// else: Impossible
#endif //
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
  debug((5, "iobuf_write: before write(%d, , %u)...\n", fd, len));
  rc = write(fd, iobuf_data_addr(bp), len);
  if(rc > 0){
    iobuf_remove(bp, rc);
  }
  debug((5, "iobuf_write(0x%0x, %d) == %d\n", (unsigned)bp, fd, rc));
  return rc;
}

int
iobuf_read(struct iobuf_t *bp, int fd)
{
  ssize_t rc = 0;
  size_t len = iobuf_free_len(bp);

  if(len){
    rc = read(fd, iobuf_free_addr(bp), len);
    if(rc > 0){
      iobuf_append(bp, rc);
    }
  }
  debug((5, "iobuf_read(0x%0x, %d) == %d\n", (unsigned)bp, fd, rc));
  return rc;
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
  debug((5, "iobuf_free_len(0x%0x) returns %d\n", (unsigned)bp, len));
  return len;
}

size_t
iobuf_data_len(const struct iobuf_t *bp)
{
  size_t len;
  if(bp->data < bp->free)
    len = bp->free - bp->data;
  else{
    if(bp->data == bp->buff + BUFSIZE)
      len = bp->free - bp->buff;
    else
      len = bp->buff + BUFSIZE - bp->data;
  }
  debug((5, "iobuf_data_len(0x%0x) returns %d\n", (unsigned)bp, len));
  return len;
}

/*
 * add new data into buffer
 */
void
iobuf_append(struct iobuf_t *bp, size_t len)
{
  debug((5, "iobuf_append(0x%0x, %u)\n", (unsigned)bp, len));
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
client_init(struct client_t *cc)
{
  cc->fail_cnt = 0;
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
_debug(int level, char *fmt, ...)
{
  va_list ap;

  if(level <= debug_level){
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
  }
}

/*
 * struct sockaddr_in *
 * parse_addr(const char *addr, const char *port)
 *     addr can be in form of comma-separated list of names and/or 
 *     addresses (i.e. server1.off,1.2.3.4,foo.com)
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
  char *caddr, *ap, *cntx;

  caddr = alloca(strlen(addr)+1);
  assert(caddr);
  strcpy(caddr, addr);
  p_addr = calloc(MAXDSTIP+1, sizeof(*p_addr));
  assert(p_addr);

  /* determine the outgoing address and port */
  if(!_port){
    syslog(LOG_ERR, "invalid target port");
    exit(EXIT_FAILURE);
  }

  for(cnt = 0, ap = strtok_r(caddr, ",", &cntx) ;
      ap ;
      ap = strtok_r(0, ",", &cntx)){

    debug((4, "parse_addr: ap = ``%s''\n", ap));
    
    if(inet_addr(ap) != INADDR_NONE){
      /* the address is in the dotted form */ 
      p_addr[cnt].sin_family = AF_INET;
      p_addr[cnt].sin_port = _port;
      p_addr[cnt].sin_addr.s_addr = inet_addr(ap);
      ++cnt;
    }else{
      char **cp;
      struct hostent *n;

      if ((n = gethostbyname(ap)) == NULL) {
	syslog(LOG_ERR, "gethostbyname failed %m");
	exit(EXIT_FAILURE);
      }
    
      for(cp = n->h_addr_list ; *cp && cnt < MAXDSTIP ; ++cp, ++cnt){
	memcpy((char *)&p_addr[cnt].sin_addr.s_addr, *cp, n->h_length);
	p_addr[cnt].sin_family = AF_INET;
	p_addr[cnt].sin_port = _port;
      }
    }
  }
  p_addr[cnt].sin_addr.s_addr = INADDR_NONE;
  p_addr_cnt = cnt;
  return p_addr;
}

/*
 *
 */
struct sockaddr_in *
get_addr(struct sockaddr_in *p_addr)
{
  if(p_addr_curr == p_addr + p_addr_cnt)
    p_addr_curr = p_addr;
  return p_addr_curr++;
}

/*
 *
 */
void
client_interp(struct client_t *cc) 
{
  size_t l;
  switch(cc->state){
  case S_CONNECT:
    break;
  case S_INIT:
    if((l = iobuf_data_len(&cc->csock.buf)) >= MAGIC_LEN + MAGIC_0_LEN){
      if(memcmp(cc->csock.buf.data+MAGIC_LEN, MAGIC_0, MAGIC_0_LEN) == 0){
	cc->state = S_RUN;
	cc->csock.buf.data += MAGIC_LEN;
      }else
	cc->state = S_RUN;
    }
    break;
  case S_MAGIC:
    /* this state is obsoleted */
    break;
  case S_RUN:
    break;
  }
}

void
client_connect_to_dst(struct client_t *cc, SOCKET csock)
{
  SOCKET osock;
  int try_again;

  /* connect a socket to the outgoing host/port */
  if ((osock = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
    syslog(LOG_ERR, "outgoing socket failed %m");
    closesocket(csock);
  } else if (bind(osock, (struct sockaddr *)&laddr, sizeof(laddr))) {
    syslog(LOG_ERR, "outgoing bind failed %m");
    closesocket(csock);
    closesocket(osock);
  } else {
    struct sockaddr_in *to_addr;

    if(fcntl(csock, F_SETFL, O_NONBLOCK) == -1){
      syslog(LOG_ERR, "fcntl(csock, F_SETFL, O_NONBLOCK) failed");
    }
    if(fcntl(osock, F_SETFL, O_NONBLOCK) == -1){
      syslog(LOG_ERR, "fcntl(osock, F_SETFL, O_NONBLOCK) failed");
    }

    cc->csock.s = csock;
    register_socket(csock, cc);
    cc->state = S_CONNECT;

    cc->osock.s = osock;
    register_socket(osock, cc);

    do {
      try_again = 1;
      to_addr = get_addr(p_addr);
      debug((1, "connecting to: %s:%d\n", inet_ntoa(to_addr->sin_addr), ntohs(to_addr->sin_port)));
      if(connect(osock, (struct sockaddr *)to_addr, sizeof(*to_addr)) == -1){
	if(errno == EINPROGRESS){
	  debug((2, "client built (delayed connection): (%d,%d)\n", csock, osock));
	  client_append_to_active_list(cc);
	  try_again = 0;
	}else{
	  if(++cc->fail_cnt >= p_addr_cnt){ 
	    /* A fatal error encountered - number of failed attemts is equal to
	       the number of available destination addresses.

	       The cc structure does not need to be freed
	       because it has not been appended to the active list and still is in
	       the free list.
	    */
	    syslog(LOG_ERR, "connect(osock,....) failed. %m");
	    closesocket(csock);
	    closesocket(osock);
	    try_again = 0;
	  }
	}
      }else{
	debug((2, "client built (instant connection): (%d,%d)\n", csock, osock)); 
	client_append_to_active_list(cc);
	try_again = 0;
	client_connected(cc);
      }
    } while(try_again);
  }
}

void
client_connected(struct client_t *cc)
{
#if SEND_CLIENT_IP
  struct sockaddr_in s_addr;
  socklen_t len = sizeof(s_addr);

  if(getpeername(cc->csock.s, (struct sockaddr *)&s_addr, &len) == 0){
    if(iobuf_data_len(cc->osock.obuf) == 0){
      /* insert MAGIC packet only if it has not been previously inserted.
         That could happen if our previous connect attemp at this client socket had failed
      */
      iobuf_push(cc->osock.obuf, MAGIC_0, MAGIC_0_LEN);
      iobuf_push(cc->osock.obuf, (char *)&s_addr.sin_addr, 4);
      iobuf_push(cc->osock.obuf, (char *)&s_addr.sin_port, 2);
      iobuf_push(cc->osock.obuf, MAGIC_1, MAGIC_1_LEN);
    }
  }
#endif

  cc->state = S_INIT;
  cc->activity = now + socket_timeout;
}

int
register_socket(int fd, struct client_t *cc)
{
  assert(0 <= fd && fd < socket_index_size);
  socket_index[fd] = cc;

  return 1;
}

void
on_term(int ignore)
{
  goon = 0;
  syslog(LOG_ERR, "signal %d caught", ignore);
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
