/*
 * IDS encrypt procedure
 */

#include <sys/types.h>

static unsigned char key[] = "LtUV*4d7*1cvj&G*23&b6%#^";
#define KEYLEN (sizeof(key)-1)

char *
IDS_Decrypt(const char *src, char *buf, size_t size)
{
  size_t i, KeyPos;
  char *dest = buf;

  *dest = '\0';
  for (i = size; i >= 1; i--) {
    KeyPos = i;
    if (KeyPos > KEYLEN) {
      KeyPos %= KEYLEN;
    }

    *dest++ = 256 - (src[i-1] ^ key[KeyPos-1]);
  }
  return buf;
}
