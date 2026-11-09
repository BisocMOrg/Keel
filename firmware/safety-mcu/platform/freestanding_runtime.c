/* C runtime of the -nostdlib images: memset and memcpy, which GCC may emit
 * calls to, the __libc_init_array hook called by the ST startup code, and an
 * empty SystemInit for images linked without the ST system file (unit tests). */
#include <stddef.h>
void *memset(void *p,int v,size_t n){unsigned char *q=p;while(n--)*q++=(unsigned char)v;return p;}
void *memcpy(void *d,const void *s,size_t n){unsigned char *a=d;const unsigned char*b=s;while(n--)*a++=*b++;return d;}
void __libc_init_array(void){}
#ifndef USE_ST_SYSTEM
void SystemInit(void){}
#endif
