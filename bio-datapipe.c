/*
 *
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <openssl/bio.h>
#include <openssl/err.h>

static void
usage(const char *me)
{
  fprintf(stderr, "Usage: %s  hostname:port\n", me);
  exit(1);
}

int
main(int argc, char **argv)
{
  char *host;
  BIO *out;
  char buf[1024*10],*p;
  int i,len,off,ret=1;

  if (argc <= 1)
    usage(argv[0]);
  else
    host = argv[1];

  /* Lets use a connect BIO under the SSL BIO */
  out = BIO_new(BIO_s_connect());
  BIO_set_nbio(out,1);
  BIO_set_conn_hostname(out, host);

  p = "GET / HTTP/1.0\r\n\r\n";
  len = strlen(p);

  off = 0;
  for (;;){
    i = BIO_write(out, p+off, len);
    if (i <= 0){
      if (BIO_should_retry(out)){
	/*	fprintf(stderr, "write DELAY\n"); */
	/*	sleep(1); */
	continue;
      }else{
	goto err;
      }
    }
    off += i;
    len -= i;
    if (len <= 0) 
      break;
  }

  for (;;){
    i = BIO_read(out, buf, sizeof(buf));
    if (i == 0) 
      break;
    if (i < 0){
      if (BIO_should_retry(out)){
	fprintf(stderr,"read DELAY\n");
	sleep(1);
	continue;
      }
      goto err;
    }
    fwrite(buf,1,i,stdout);
  }

  ret = 1;
  if(0){
  err:
    if (ERR_peek_error() == 0){ /* system call error */
      fprintf(stderr,"errno=%d ",errno);
      perror("error");
    }else
      ERR_print_errors_fp(stderr);
  }
  BIO_free_all(out);

  return ret;
}

// end of prog1.cc
