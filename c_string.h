/*
 *
 */

#ifndef __C_STRING_H
#define __C_STRING_H

#include <sys/types.h>

struct string_t {
  char *ptr;
  size_t size;
  size_t nmax;
};

typedef struct string_t string;

string *str_new(void);
string *str_init(string *s);
void str_delete(string *s);

void str_erase(string *s);
void str_push_back(string *s, char ch);
void str_allocate(string *s, size_t n);
string *str_copy(string *dst, const string *src);
int str_eq(const string *s1, const string *s2);
int str_eq_c(const string *s1, const char *s2, size_t n);
void str_dump(const string *s);


#endif /* __C_STRING_H */
