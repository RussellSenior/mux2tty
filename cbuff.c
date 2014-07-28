#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <syslog.h>
#include <ctype.h>

#include "cbuff.h"

int new_cbuff (struct cbuff *cb, int n) 
{
  syslog (LOG_DEBUG, "new_cbuff: allocating new cbuff of size %d",n);
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
  syslog (LOG_DEBUG, "free_cbuff: freeing cbuff of size %d with %d remaining unused",cb->len,cb->left);
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
  syslog (LOG_DEBUG, "resize_cbuff: resizing cbuff from size %d to %d",cb->len,n);

  int csize = cb->len - cb->left;
  if (csize > n) {
    syslog (LOG_DEBUG, "cannot shrink buffer with %d bytes to %d",csize,n);
    return -1;
  }

  char* new_buff = (char *) malloc (n);
  if (!new_buff) {
    syslog (LOG_ERR, "resize allocation of %d bytes failed",n);
    return -1;
  }

  syslog (LOG_DEBUG, "copying %d bytes of content to new buffer",csize);
  for (int i=0 ; i<csize ; i++) {
    new_buff[i] = cb->buff[(cb->start + i) % cb->len];
  }
  cb->start = 0;
  cb->end = csize;
  cb->len = n;
  cb->left = n - csize;
  syslog (LOG_DEBUG, "freeing old buffer");
  free(cb->buff);
  cb->buff = new_buff;
  return 0;
}

int dump_cbuf(struct cbuff *cb, int count) 
{
  fprintf(stderr, "[");
  for (int i = 0 ; i < count ; i++)
    fprintf(stderr, " %02x", cb->buff[(cb->start + i) % cb->len]);
  fprintf(stderr,"]\n[");
  for (int i = 0 ; i < count ; i++) {
    char c = cb->buff[(cb->start + i) % cb->len];
    if (isprint(c)) 
      fprintf(stderr, "  %c",c);
    else
      fprintf(stderr, "   ");
  }
  fprintf(stderr, "]\n");
}

int read2cbuf (struct cbuff *cb, int fd) 
{
  // note that to simplify implementation, this function only fills buffer to the
  // end and then returns, leaving to the next read filling any unused portion of
  // the buffer at the beginning.  Since the input will be ready for reading, the
  // select call will trigger without too much delay.
  syslog (LOG_DEBUG, "read2cbuf: reading from fd %d into buffer", fd);
  if (!cb->left) {
    syslog (LOG_ERR, "no space in buffer");
    return -1;
  }

  syslog (LOG_DEBUG, "before read, start = %d ; end = %d ; len = %d ; left = %d", cb->start, cb->end, cb->len, cb->left);
  int count = read (fd, cb->buff + cb->end, 
		    ((cb->end < cb->start) ? cb->start : cb->len) - cb->end);
  if (count > 0) {
    cb->left -= count;
    cb->end += count;
    if (cb->end == cb->len)
      cb->end = 0;
  }
  dump_cbuf(cb,count);
  syslog (LOG_DEBUG, "after read, start = %d ; end = %d ; len = %d ; left = %d", cb->start, cb->end, cb->len, cb->left);
  syslog (LOG_DEBUG, "%d bytes read", count);
  return count;
}

int cbuf2write (struct cbuff *cb, int fd, int n)
{
  syslog (LOG_DEBUG, "cbuf2write: writing %d bytes to fd %d from buffer", n, fd);

  // if cbuff is empty, should never happen
  if (cb->left == cb->len) {
    syslog (LOG_ERR, "buffer of %d bytes is empty", cb->len);
    return -1;
  }

  int m = n;
  // keep writing until writes don't write, then return bytes written
  // caller must deal with partial write.
  while (m) {
    int o = (cb->end <= cb->start) ? 
      cb->len - cb->start :
      cb->end - cb->start;
    syslog (LOG_DEBUG, "before write, start = %d ; end = %d ; len = %d ; left = %d", cb->start, cb->end, cb->len, cb->left);
    int count = write (fd, cb->buff + cb->start, (m < o) ? m : o);
    dump_cbuf(cb,count);
    if (count > 0) {
      m -= count;
      cb->start += count;
      cb->left += count;
      if (cb->start == cb->len)
	cb->start = 0;
      syslog (LOG_DEBUG, "after write, start = %d ; end = %d ; len = %d ; left = %d", cb->start, cb->end, cb->len, cb->left);
    } else {
      syslog (LOG_DEBUG, "write returned 0 bytes, returning %d total bytes written", n-m);
      return n - m;
    }
  }
  syslog (LOG_DEBUG, "wrote %d bytes", n);
  return n;
}

int cbuf2buf (struct cbuff *cb, char *dest, int n)
{
  syslog (LOG_DEBUG, "cbuf2buf: reading %d bytes from buffer into a scratch buffer", n);
  syslog (LOG_DEBUG, "before copy, start = %d ; end = %d ; len = %d ; left = %d", cb->start, cb->end, cb->len, cb->left);
  for (int i=0 ; i<n ; i++) {
    dest[i] = cb->buff[(cb->start + i) % cb->len];
  }
  cb->start = (cb->start + n) % cb->len;
  cb->left += n;
  syslog (LOG_DEBUG, "after copy, start = %d ; end = %d ; len = %d ; left = %d", cb->start, cb->end, cb->len, cb->left);
  return n;
}

int buf2cbuf (struct cbuff *cb, char *src, int n)
{
  syslog (LOG_DEBUG, "buf2cbuf: reading %d bytes from scratch buffer into buffer", n);
  syslog (LOG_DEBUG, "before copy, start = %d ; end = %d ; len = %d ; left = %d", cb->start, cb->end, cb->len, cb->left);
  if (cb->left < n)
    n = cb->left;
  for (int i=0 ; i<n ; i++) {
    cb->buff[(cb->end + i) % cb->len] = src[i];
  }
  cb->end = (cb->end + n) % cb->len;
  cb->left -= n;
  syslog (LOG_DEBUG, "after copy, start = %d ; end = %d ; len = %d ; left = %d", cb->start, cb->end, cb->len, cb->left);
  return n;
}
	
int cbuf_find (struct cbuff *cb, char c) 
{
  if (isprint (c))
    syslog (LOG_DEBUG, "cbuf_find: looking in buffer for \'%c\' 0x%x", c,c);
  else
    syslog (LOG_DEBUG, "cbuf_find: looking in buffer for 0x%x", c);
  int csize = cb->len - cb->left;
  for (int i=0 ; i<csize ; i++)
    if (cb->buff[(cb->start + i) % cb->len] == c) {
      syslog (LOG_DEBUG, "found delimiter %d bytes from start index", i+1);
      return i+1;
    }
  syslog (LOG_DEBUG, "did not find delimiter in buffer");
  return 0;
}

int cbuf_findtiu (struct cbuff *cb)
{
  syslog (LOG_DEBUG, "cbuf_findtiu: searching buffer for EOD");
  return cbuf_find (cb, 0x4d);
}

int cbuf_finduit (struct cbuff *cb)
{
  syslog (LOG_DEBUG, "cbuf_finduit: return %d bytes available", cb->len - cb->left);
  return (cb->len - cb->left);
}
