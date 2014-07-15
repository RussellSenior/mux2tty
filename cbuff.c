#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cbuff.h"

int new_cbuff (struct cbuff *cb, int n) 
{
  printf ("allocating new cbuff of size %d\n",n);
  cb->buff = (char *) malloc (n);
  if (!cb->buff)
    return -1;

  memset (cb->buff, 0, n);

  cb->start = 0;
  cb->end = 0;
  cb->len = n;
  cb->left = n;
  return 0;
}
 
int free_cbuff (struct cbuff *cb) {
  printf ("freeing cbuff of size %d with %d remaining unused\n",cb->len,cb->left);
  if (cb->buff)
    free(cb->buff);
  cb->buff = NULL;
  cb->start = 0;
  cb->end = 0;
  cb->len = 0;
  cb->left = 0;
  return 0;
}

int resize_cbuff (struct cbuff *cb, int n) {
  int csize = cb->len - cb->left;
  if (csize > n)
    return -1;

  char* new_buff = (char *) malloc (n);
  if (!new_buff) 
    return -1;

  for (int i=0 ; i<csize ; i++) {
    new_buff[i] = cb->buff[(cb->start + i) % cb->len];
  }
  cb->start = 0;
  cb->end = csize;
  cb->len = n;
  cb->left = n - csize;
  free(cb->buff);
  cb->buff = new_buff;
  return 0;
}

int read2cbuf (struct cbuff *cb, int fd) 
{
  if (!cb->left)
    return -1;

  int count = read (fd, cb->buff + cb->end, 
		    ((cb->end < cb->start) ? cb->start : cb->len) - cb->end);
  if (count > 0) {
    cb->left -= count;
    cb->end += count;
    if (cb->end == cb->len)
      cb->end = 0;
  }
  return count;
}

int cbuf2write (struct cbuff *cb, int fd, int n)
{
  // if cbuff is empty, should never happen
  if (cb->left == cb->len)
    return -1;
  
  int m = n;
  // keep writing until writes don't write, then return bytes written
  // caller must deal with partial write.
  while (m) {
    int o = (cb->end <= cb->start) ? 
      cb->len - cb->start :
      cb->end - cb->start;
    int count = write (fd, cb->buff + cb->start, (m < o) ? m : o);
    if (count > 0) {
      m -= count;
      cb->start += count;
      cb->left += count;
      if (cb->start == cb->len)
	cb->start = 0;
    } else {
      return n - m;
    }
  }
  return n;
}

int cbuf2buf (struct cbuff *cb, char *dest, int n)
{
  for (int i=0 ; i<n ; i++) {
    dest[i] = cb->buff[(cb->start + i) % cb->len];
  }
  cb->start = (cb->start + n) % cb->len;
  cb->left += n;
  return n;
}

int buf2cbuf (struct cbuff *cb, char *src, int n)
{
  if (cb->left < n)
    n = cb->left;
  for (int i=0 ; i<n ; i++) {
    cb->buff[(cb->end + i) % cb->len] = src[i];
  }
  cb->end = (cb->end + n) % cb->len;
  cb->left -= n;
  return n;
}
	
int cbuf_find (struct cbuff *cb, char c) 
{
  int csize = cb->len - cb->left;
  for (int i=0 ; i<csize ; i++)
    if (cb->buff[(cb->start + i) % cb->len] == c)
      return i+1;
  return 0;
}

int cbuf_findtiu (struct cbuff *cb)
{
  return cbuf_find (cb, 0x4d);
}

int cbuf_finduit (struct cbuff *cb)
{
  if ((cb->len - cb->left) > 0)
    return 1;
}
