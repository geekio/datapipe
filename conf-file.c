#include <stdio.h>
#include <string.h>
#include <syslog.h>
#include <sys/types.h>
#include <ctype.h>
#include <stdlib.h>

#include "conf-file.h"

const char *local_ip = "0";
const char *local_port = "11005";
const char *default_remote_port = "11005";
const char *local_out_ip = 0; 

//
// prototypes
//

ssize_t pack(const char *zstr, char *data, size_t data_size);

/*
 * parses the config file
 * Returns:
 *  0 if Ok
 *  error message
 */
const char * 
config(const char *cfg_file)
{
  FILE *cfg;
  char str[120];
  size_t n_line = 0;
  const char *t = 0;

  cfg = fopen(cfg_file, "r");
  if(!cfg){
    syslog(LOG_NOTICE, "Can't open config file %s. %m", cfg_file);
    t = "config file open error";
  }else{
    while(t == 0 && fgets(str, sizeof(str), cfg)){
      char *rest, *cp = strchr(str, '\n');
      char *key, *val;

      n_line++;
      if(!cp){
	t = "NL not found. Line is too long";
	break;
      }
      *cp = 0; // chop \n
      key = strtok_r(str, " ", &rest);
      val = strtok_r(0, " ", &rest);
      
      if(key == 0 || *key == 0 || *key == '#')
	continue;

      if(strcmp(key, "local-ip") == 0)
	local_ip = strdup(val);
      else if(strcmp(key, "local-out-ip") == 0)
	local_out_ip = strdup(val);
      else if(strcmp(key, "local-port") == 0)
	local_port = strdup(val);
      else if(strcmp(key, "remote-port") == 0)
	default_remote_port =  strdup(val);
      else if(strcmp(key, "login-type") == 0){
	char bin[1];
	if(pack(val, bin, sizeof(bin)) == -1)
	  t = "bad login-type type";
	else
	  t = on_login_type(bin[0] & 255);
	//	printf("%d 0x%x\n", bin[0]&255, bin[0]&255);
      }else if(strcmp(key, "name") == 0){
	char *prefix = val;
	char *dst_addr = strtok_r(0, " ", &rest);
	if(dst_addr){
	  int rv;
	  char bin[MAXPREFIXSIZE], *dst_port;

	  dst_port = strtok_r(0, " ", &rest);
	  if(dst_port == 0)
	    dst_port = strdup(default_remote_port);
	  if((rv = pack(prefix, bin, MAXPREFIXSIZE)) == -1){
	    syslog(LOG_NOTICE, "At %d: Bad prefix string", n_line);
	    t = "name: bad prefix string";
	  }else{
	    on_prefix(rv, bin, dst_addr, dst_port);
	  }
	}else{
	  syslog(LOG_NOTICE, "At %d: No dst address in name statement", n_line);
	  t = "name: dst addr missing";
	}
      }else if(strcmp(key, "name-default") == 0){
	char *dst_addr = val;
	if(dst_addr){
	  char *dst_port;
	  
	  dst_port = strtok_r(0, " ", &rest);
	  if(dst_port == 0)
	    dst_port = strdup(default_remote_port);
	
	  on_prefix(0, 0, dst_addr, dst_port); // special case -- default address
	}else{
	  syslog(LOG_NOTICE, "At %d: No dst address in name-default statement", n_line);
	  t = "name: dst addr missing";
	}
      }else if(strcmp(key, "type") == 0){
	char *prefix = val;
	char *dst_addr = strtok_r(0, " ", &rest);
	if(dst_addr){
	  int rv;
	  char bin[1], *dst_port;

	  dst_port = strtok_r(0, " ", &rest);
	  if(dst_port == 0)
	    dst_port = strdup(default_remote_port);
	  if((rv = pack(prefix, bin, 1)) == -1){
	    syslog(LOG_NOTICE, "At %d: Bad prefix string", n_line);
	    t = "name: bad prefix string";
	  }else{
	    on_type((unsigned char)bin[0], dst_addr, dst_port);
	  }
	}else{
	  syslog(LOG_NOTICE, "At %d: No dst address in name statement", n_line);
	  t = "name: dst addr missing";
	}
      }else{
	syslog(LOG_NOTICE, "At %d: %s - Unknown statement", n_line, key);
	t = "Unknown statement";
      }
    }
    fclose(cfg);
  }
  return t;
} 

static int
hex_digit(int h)
{
  switch(h&255){
  case '0': return 0;
  case '1': return 1;
  case '2': return 2;
  case '3': return 3;
  case '4': return 4;
  case '5': return 5;
  case '6': return 6;
  case '7': return 7;
  case '8': return 8;
  case '9': return 9;
  case 'A': case 'a': return 10;
  case 'B': case 'b': return 11;
  case 'C': case 'c': return 12;
  case 'D': case 'd': return 13;
  case 'E': case 'e': return 14;
  case 'F': case 'f': return 15;
  }
  abort();
}

//
// returns -1 if error
// else actual unpacked string
//
//
ssize_t pack(const char *zstr, char *data, size_t data_size)
{
  char *op = data;
  const char *ip = zstr;
  int state = 0;

  while(*ip && (op - data) < data_size){
    switch(state){
    case 0: // initial state
      if(*ip == '\\')
	state = 1;
      else
	*op++ = *ip;
      break;
    case 1: // '\\' seen
      if(*ip == 'x')
	state = 2; // 2 hex digits
      else if(*ip == 'n'){
	state = 0;
	*op++ = '\n';
      }else if(*ip == '\\'){
	state = 0;
	*op++ = '\\';
      }else
	return -1;
      break;
    case 2: // first hex digit
      if(!isxdigit(*ip))
	return -1;
      state = 3;
      *op = hex_digit(*ip);
      break;
    case 3:
      *op = (*op << 4) + hex_digit(*ip);
      op++;
      state = 0;
      break;
    }
    ip++;
  }
  return state == 0 ? op - data : -1;
}

#ifdef TEST_PACK

#include <stdio.h>

int
main()
{
  char up[] = "\\x30\\x6A\\x32\\x33\\x34\\x35\\x7b\\n";
  char bin[10];

  ssize_t rv = pack(up, bin, sizeof(bin) - 1);
  printf("rv == %d\n", rv);
  if(rv > 0){
    bin[rv] = 0;
    printf("%s -> %s\n", up, bin);
  }
}

void
on_prefix(const char *name, const char *dst_addr, const char *dst_port)
{
}

#endif // TEST_PACK
