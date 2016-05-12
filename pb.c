/* dataPipe Benchmark */

#include <unistd.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>

#include <errno.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <openssl/bio.h>
#include <openssl/err.h>

/* -------------------- timer.h --------------------- */
#include <sys/time.h>

typedef double pb_timer_t;

struct Timer {
  struct timeval _start;
  struct timeval _stop;
};

void 
Timer_zero(struct timeval *tv) 
{ 
  tv->tv_sec = 0; 
  tv->tv_usec = 0; 
};

inline pb_timer_t 
Timer_d(struct timeval *tv) 
{ 
  return tv->tv_sec*1000000. + tv->tv_usec; 
};

void Timer_start(struct Timer *this) { gettimeofday(&this->_start, 0); };
void Timer_stop(struct Timer *this) { gettimeofday(&this->_stop, 0); };

void 
Timer(struct Timer *this) 
{ 
  Timer_zero(&this->_start);
  Timer_zero(&this->_stop);
};

pb_timer_t 
Timer_elapsed(struct Timer *this) 
{ 
  return (Timer_d(&this->_stop) - Timer_d(&this->_start)) / 1000000.; 
};

/* -------------------- timer.h --------------------- */

int hz_decode(const char *from, char *buff);
void hz_encode(const char *from, size_t cnt);

static char *drones[] = {
  " $5E $0B $00 $0E $01 $81 $BA $9A $B6 $E5 $A6 $F5 $B9 $B6 $5E",
  " $5E $0B $00 $0E $01 $81 $BA $9A $B6 $E4 $A6 $F5 $B9 $B6 $5E",
  " $5E $0B $00 $0E $01 $81 $BA $9A $B6 $E7 $A6 $F5 $B9 $B6 $5E"
};

static char *login = 0;

#define ANSWER  "\x01\x02\x00\x65\x8E\x01"
#define ANSWER_LEN (sizeof(ANSWER)-1)

/* command line flags */

size_t n_tests = 1;
size_t concurrency = 1;
int keep_flag = 0;
int random_delay_flag = 0;

void
usage(const char *me)
{
  fprintf(stderr, "Usage: %s [flags] host:port\n", me);
  fprintf(stderr, "\t-1 Login as drone1 (default)\n");
  fprintf(stderr, "\t-2 Login as drone2\n");
  fprintf(stderr, "\t-3 Login as drone3\n");
  fprintf(stderr, "\t-r Random delay before fork\n");
  fprintf(stderr, "\t-nNNN Set number of loops (tests)\n");
  fprintf(stderr, "\t-cNNN Set number of concurrent processes\n");
  fprintf(stderr, "\t-k Keep established connections\n");
  exit(1);
}

void
test(const char *target)
{
  int k;
  for(k = 0 ; k < n_tests ; k++){
    BIO *out;
    char buf[512], *cp;
    int n;
    int rc;

    out = BIO_new(BIO_s_connect());
    BIO_set_conn_hostname(out, target);
    if(BIO_do_connect(out) == 1){
      int hz_len;
      //    fprintf(stderr, "connected to %s (fd=%ld)\n", target, BIO_get_fd(out, 0)); // DEBUG
      hz_len = hz_decode(login, buf);
      //    hz_encode(buf, hz_len); // DEBUG
      assert(hz_len > 0 && hz_len < sizeof(buf)); // DEBUG
      rc = BIO_write(out, buf, hz_len);
      assert(rc == hz_len);
      cp = buf;
      n = sizeof(buf);
      while((rc = BIO_read(out, cp, n)) > 0 && cp - buf < ANSWER_LEN){
	//      hz_encode(cp, rc);
	cp += rc;
	n -= rc;
      }
      if(memcmp(buf, ANSWER, ANSWER_LEN)){
	fprintf(stderr, "Bad answer:\n");
	hz_encode(buf, cp - buf);
      }else{
	printf("Ok...\n"); 
      }
    }else{
      if (ERR_peek_error() == 0){ /* system call error */
	fprintf(stderr, "errno=%d ", errno);
	perror("error");
      }else{
	perror("error");
	ERR_print_errors_fp(stderr);
      }
    }
    BIO_free_all(out);
  }
  fprintf(stderr, "Finished...(pid = %d)\n", getpid());
}

long
random_n(long low, long up)
{
  const double rev_max_random = 1.0 /((1U<<31)-1);

  return (long)(low + (random() * rev_max_random * (up - low) + 0.5));
}

void
run_tests(const char *target, size_t n)
{
  fprintf(stderr, "run_tests(%s, %u)\n", target, n); // DEBUG
  while(n--){
    int rc;
    switch((rc = fork())){
    case 0:
      /* run the test */
      if(random_delay_flag)
	usleep(random_n(100, 500)*1000);
      test(target);
      exit(0);
    case -1:
      /* fork error */
      perror("fork");
      exit(1);
    default:
      /* Just continue and start next */
      ;
    }
  }
  while(waitpid(-1, 0, 0) != -1)
    ;
  if(errno != ECHILD)
    perror("waitpid");
};

void
test_random(int n)
{
  while(n--){
    printf("%ld ", random_n(1,10));
  }
  printf("\n");
}

int
main(int argc, char **argv)
{
  char *me = strdup(argv[0]);
  int ch;
  struct Timer t;
  Timer(&t); // Init -- C++ constructor like... */

  login = drones[0]; /* set the default */

  while ((ch = getopt(argc, argv, "ST:123hn:c:kr")) != -1) {
    switch (ch) {
    case 'S':
      srandomdev();
      break;
    case 'T':
      test_random(atoi(optarg));
      exit(0);
    case 'r':
      random_delay_flag = 1;
      break;
    case '1': case '2': case '3':
      login = drones[ch - '1'];
      break;
    case 'k':
      keep_flag = 1;
      break;
    case 'n':
      n_tests = atoi(optarg);
      break;
    case 'c':
      concurrency = atoi(optarg);
      break;
    case '?':
    case 'h':
    default:
      usage(me);
    }
  }
  argc -= optind;
  argv += optind;

  if(argc != 1)
    usage(me);

  Timer_start(&t);
#if 1
  run_tests(argv[0], concurrency);
#else
  test(argv[0]);
#endif // 0
  Timer_stop(&t);
  fprintf(stderr, "\nTime elapsed: %g sec.\n", Timer_elapsed(&t));

  return 0;
}

int
ch2hex(int ch)
{
  int x;

  switch((ch = tolower(ch))){
  case '0':  case '1':
  case '2':  case '3':
  case '4':  case '5':
  case '6':  case '7':
  case '8':  case '9':
    x = ch - '0'; break;
  case 'a':  case 'b':
  case 'c':  case 'd':
  case 'e':  case 'f':
    x = ch - 'a' + 10; break;
  }
  return x;
}

void
hz_encode(const char *from, size_t cnt)
{
  while(cnt--)
    printf(" $%02X", *from++&255);
  printf("\n");
}

int
hz_decode(const char *from, char *buff)
{
  int state = 0;
  int ch, x;
  char *out;

  ch = *from++;
  out = buff;
  while(ch){
    switch(state){
    case 0:
      if(ch == ' ')
	state = 1;
      break;
    case 1:
      if(ch == '$')
	state = 2;
      break;
    case 2:
      if(isxdigit(ch)){
	x = ch2hex(ch);
	state = 3;
      }
      break;
    case 3:
      if(isxdigit(ch)){
	x = x * 16 + ch2hex(ch);
	state = 0;
	*out++ = x;
      }
      break;
    }
    ch = *from++;
  }
  return out - buff;
}

#if 0
Login:
 $5E $0B $00 $0E $01 $81 $BA $9A $B6 $E5 $A6 $F5 $B9 $B6 $E1 $B6 $77 $CD
 $AB $BA $DC $F4 $FB $12 $00 $1E $3D $E1 $77 $F2 $E1 $B6 $77 $2A $05 $1D $00 $02
 $04 $00 $00 $03 $00 $00 $00 $00 $00 $00 $00 $02 $04 $00 $00 $1C $CB $DD $00 $14
 $FC $12 $00 $9A $3D $E1 $77 $79 $04 $FF $FF $2A $05 $1D $00 $02 $04 $00 $00 $03
 $00 $00 $00 $00 $00 $00 $00 $01 $00 $00 $00 $64 $FD $12 $00 $EC $B5 $44 $00 $79
 $04 $FF $FF $2A $05 $1D $00 $02 $04 $00 $00 $03 $00 $00 $00 $00 $00 $00 $00 $64
 $FD $12 $00 $00 $00 $00 $C0 $1C $CB $DD $00 $BC $FD $12 $00 $78 $85 $44 $00 $00
 $00 $00 $C0 $BC $FD $12 $00 $1C $CB $DD $00 $1C $CB $DD $00 $90 $FC $12 $00 $08
 $B5 $44 $00 $C1 $0A $EA $00 $28 $05 $23 $00 $0F $00 $00 $00 $E0 $3E $1B $00 $3C
 $FE $12 $00 $30 $C2 $46 $00 $08 $38 $DC $00 $43 $83 $44 $00 $08 $38 $DC $00 $34
 $B0 $00 $00 $00 $00 $00 $00
Accept:
 $01 $02 $00 $65 $8E $01

5E 0B 00 0E 01 81 BA 9A B6 E5 A6 F5 B9 B6 5E

dron1
5E 0B 00 0E 01 81 BA 9A B6 E5 A6 F5 B9 B6 5E 

dron2
5E 0B 00 0E 01 81 BA 9A B6 E4 A6 F5 B9 B6 5E 

dron3
5E 0B 00 0E 01 81 BA 9A B6 E7 A6 F5 B9 B6 5E 



#endif // 0

