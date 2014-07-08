struct cbuff {
  char* buff;
  int start;
  int end;
  int len;
  int left;
};

int new_cbuff (struct cbuff *cb, int n);
int free_cbuff (struct cbuff *cb);
int resize_cbuff (struct cbuff *cb, int n);
int read2cbuf (struct cbuff *cb, int fd);
int cbuf2write (struct cbuff *cb, int fd, int n);
int cbuf2buf (struct cbuff *cb, char* buf, int n);
int cbuf_find (struct cbuff *cb, char c);
