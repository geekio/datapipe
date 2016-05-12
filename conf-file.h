/* 
*/
#ifndef __CONF_FILE_H
#define __CONF_FILE_H

#define MAXPREFIXSIZE 64

extern const char *local_ip;
extern const char *local_port;
extern const char *default_remote_port;
extern const char *local_out_ip;

void on_prefix(size_t size, const char *pr, const char *dst_addr, const char *dst_port);
void on_type(int type, const char *dst_addr, const char *dst_port);
const char *on_login_type(int type);
const char *config(const char *cfg_file);

#endif // __CONF_FILE_H
