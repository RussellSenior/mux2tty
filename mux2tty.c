#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <getopt.h>
#include <string.h>

#include <sys/stat.h>
#include <fcntl.h>
#include <termios.h>

#include <sys/time.h>
#include <sys/select.h>
#include <signal.h>

#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netdb.h>

#include <time.h>

#include "cbuff.h"

int verbose = 0;

struct termios tp, save;

int print_usage (char* progname)
{
  printf("%s usage:\n"
	 "-h,--help        this message\n"
	 "-t,--tty <arg>   attach to <arg> tty\n"
	 "-b,--baud <arg>  communicate with tty at <arg> baud\n"
	 "-p,--port <arg>  listen for tcp connections on port <arg>\n"
	 "-v,--verbose     describe what is happening\n"
	 ,progname);
}

int max_fds(fd_set *set,int start) 
{
  int max = start ? start : FD_SETSIZE;
  while (max > 0 && !FD_ISSET(max-1, set))
    max--;
  return max;
}

int max_fds2(fd_set *set1, fd_set *set2, int start)
{
  int max = start ? start : FD_SETSIZE;
  while (max > 0 && !FD_ISSET(max-1, set1) && !FD_ISSET(max-1, set2))
    max--;
  return max;
}

int validate_terminal(char*,char*);
int validate_port(char*);

int main(int argc,char** argv) 
{
  int c;
  
  char* baudstr = NULL;
  char* portstr = NULL;
  char* termstr = NULL;
  
  while (1) 
    {
      
      static struct option long_options[] =
      {
	{"help", no_argument, 0, 'h'},
	{"port", required_argument, 0, 'p'},
	{"tty", required_argument, 0, 't'},
	{"baud", required_argument, 0, 'b'},
	{"verbose", no_argument, 0, 'v'},
	{ 0,0,0,0 }
      };
      
      int option_index = 0;
      
      c = getopt_long (argc, argv, "hb:p:t:v",
		       long_options, &option_index);

      if (c == -1)
	break;

      switch (c) 
	{
	case 'h':
	  print_usage(argv[0]);
	  return 0;

	case 't':
	  termstr = optarg;
	  break;

	case 'b':
	  baudstr = optarg;
	  break;

	case 'p':
	  portstr = optarg;
	  break;

	case 'v':
	  verbose = 1;
	  break;

	default: 
	  return -1;
	}
    }

  int term = validate_terminal(termstr,baudstr);

  if (term < 0) {
    printf("opening terminal %s at %s failed with error %d\n",termstr,baudstr,term);
    return -2;
  }

  int port = validate_port(portstr);

  if (verbose) {
    printf("terminal = %s\n",termstr);
    printf("baud = %s\n",baudstr);
    printf("tty fd = %d\n",term);
    printf("port = %s\n",portstr);
    printf("port number = %d\n",port);
  }

  struct cbuff* b = NULL;
  
#define DELIM '\n'

  int len = 0;
  int nfds = 0;

  b = (struct cbuff*) calloc (term + 1, sizeof(struct cbuff));
  if (!b) {
    printf ("failed to allocated cbuff array for tty\n");
    return -3;
  }

  if (new_cbuff(b+term,64) < 0) {
    printf ("failed to allocated cbuff buffer for tty\n");
    return -4;
  }
 
  int maxfd = port + 1;
  fd_set sessions;
  fd_set closed;
  FD_ZERO(&sessions);
  FD_ZERO(&closed);

  int last = 0;
  int pending = 0;

  while (1) {

    if (verbose) {
      printf ("tty: %d ; listening %d\n",term,port);
    }
    fd_set readfds,writefds;
    memcpy(&readfds,&sessions,sizeof(fd_set));
    FD_ZERO(&writefds); // clear output
    FD_SET(term,&readfds); // tty
    FD_SET(port,&readfds); // listening for connections

    if (pending) {
      printf ("writes to tty pending\n");
      FD_SET(term,&writefds);
    }

    for (int fd=0 ; fd<nfds ; fd++) {
      if (FD_ISSET(fd, &sessions)) {
	printf ("session %d\n",fd);
	int n = cbuf_find(b+fd,DELIM);
	if (FD_ISSET (fd, &closed)) {
	  printf ("session %d is closed, don't read\n",fd);
	  // session is closed, so don't read
	  FD_CLR(fd,&readfds);
	  if (!n) {
	    // closed session has no more complete records
	    // and won't be getting any new ones, so release
	    // and remove from future consideration
	    printf ("no complete records in closed session %d\n",fd);
	    free_cbuff(b+fd);
	    FD_CLR(fd,&sessions);
	    FD_CLR(fd,&closed);
	    printf ("freeing cbuff and removing %d from sessions and closed lists\n",fd);
	  }
	}
	if (n) {
	  printf ("session %d has %d bytes to write, checking tty for writability\n",fd,n);
	  FD_SET(term,&writefds);
	} else if (b[fd].left == 0) {
	  printf ("cbuff for session %d does not have a complete record, and is out of space\n",fd);
	  // no delimiter, buffer full, so double size
	  if (resize_cbuff(b+fd,b[fd].len * 2) < 0)
	    printf ("resize_cbuff session %d failed\n",fd);
	}
      }
    }
    
    nfds = max_fds2(&readfds,&sessions,maxfd);

    b = (struct cbuff *) realloc (b,nfds * sizeof(struct cbuff));

    int ready = select(nfds,&readfds,&writefds,NULL,NULL);

    if (verbose) 
      printf ("%d fd ready\n",ready);

    if (ready <= 0) {
      printf ("nothing readable yet, select returned %d\n",ready);
    } else {
      for (int fd = 0 ; fd < nfds ; fd++) {
	if (FD_ISSET (fd, &readfds)) {    
	  if (verbose) {
	    printf ("read fd = %d\n",fd);
	  }
	  if (fd == term) {
	    // data has arrived on tty, read into buffer
	    len = read2cbuf(b+term,term);
	    if (verbose) {
	      printf ("read %d bytes from tty\n",len);
	    }
	    if (len < 0) {
	      // error reading tty
	      printf ("error reading tty\n");
	    } else if (len == 0) {
	      // tty has closed, exit
	      for (int i=0 ; i<nfds ; i++) {
		if (FD_ISSET (i, &sessions) && !FD_ISSET (i, &closed)) {
		  close(i);
		  printf ("closed session %d\n",i);
		  FD_SET (i, &closed);
		}
	      }
	      close(port);
	      if (verbose)
		printf ("tty closed, exiting\n");
	      return 0;
	    } 
	  } else if (fd == port) {
	    // connection request on listening port
	    struct sockaddr_storage naddr;
	    int addrlen = sizeof(naddr);
	    char hostname[NI_MAXHOST];
	    char service[NI_MAXSERV];

	    if (verbose) {
	      printf ("accepting connection on port %d\n",port);
	    }

	    int nfd = accept (port, (struct sockaddr *) &naddr, &addrlen);

	    if (nfd < 0) {
	      printf ("error %d accepting connection on port %d\n",nfd,port);
	    } else {
	      FD_SET (nfd, &sessions);
	      if (nfds >= maxfd) 
		maxfd = nfds + 1;

	      nfds = max_fds(&sessions,maxfd);

	      b = (struct cbuff *) realloc (b, nfds * sizeof(struct cbuff));
	      if (!b) {
		printf("failure to allocate cbuff array for %d\n",nfd);
		return -5;
	      }

	      if (new_cbuff(b+nfd,64) < 0) {
		printf ("failed to allocated cbuff buffer for %d\n",nfd);
		return -6;
	      }

	      if (getnameinfo((struct sockaddr *) &naddr,addrlen,
			      hostname,NI_MAXHOST,
			      service,NI_MAXSERV,
			      NI_NUMERICHOST | NI_NUMERICSERV) != 0) {
		printf ("getnameinfo failed\n");
	      } else {
		printf ("connection %d from %s:%s\n",nfd,hostname,service);
	      }
	    }
	  } else {
	    // received data from a session
	    len = read2cbuf (b+fd,fd);
	    if (verbose) {
	      printf ("read %d bytes from session %d\n",len,fd);
	    }
	    if (len < 0) {
	      // error reading session
	      printf ("error reading fd %d\n",fd);
	    } else if (len == 0) {
	      // session closed
	      printf ("closing session %d\n",fd);
	      printf ("session %d cbuff contains %d bytes\n",fd,b[fd].len - b[fd].left);
	      close(fd);
	      printf ("marking session %d closed\n",fd);
	      FD_SET (fd, &closed);
	    } 
	  }
	}
      }
      // try to write
      if (FD_ISSET (term, &writefds)) {
	printf ("tty is writable\n");
	if (pending) {
	  printf ("serving pending buffer %d\n",pending);
	  int n = cbuf_find(b+pending,DELIM);
	  int len = cbuf2write(b+pending,term,n);
	  if (len == n) {
	    printf ("completed pending buffer %d\n",pending);
	    pending = 0;
	  }
	}
	printf ("pending = %d\n",pending);
	if (!pending) {
	  for (int i=0 ; i<nfds ; i++) {
	    int fd = (last + i + 1) % nfds;
	    if (FD_ISSET (fd, &sessions)) {
	      printf ("looking for delimiter in session %d buffer\n",fd);
	      int n = cbuf_find (b+fd,DELIM);
	      if (n) {
		printf("record delimter found at offset %d of buffer %d\n",n,fd);
		int len = cbuf2write(b+fd,term,n);
		printf ("wrote %d bytes to tty from session %d\n",len,fd);
		if (len > 0 && len < n) {
		  pending = fd;
		} 
		last = fd;
		printf ("last session %d\n",last);
	      } else if (b[fd].left == 0) {
		// no delimiter, buffer full, so double size
		printf ("resizing buffer for session %d\n",fd);
		if (resize_cbuff(b+fd,b[fd].len * 2) < 0)
		  printf ("resize_cbuff session %d failed\n",fd);
	      }
	    }
	  }
	}
      }
      // check tty cbuff for records, if ready, send to sessions
      printf ("looking for delimiter in tty %d buffer\n",term);
      int n = cbuf_find(b+term,DELIM);
      if (n) {
	char buf[64];
	int len = cbuf2buf (b+term,buf,n);
	printf ("copied %d of %d chars to buffer\n",len,n);
	for (int fd=0 ; fd<nfds ; fd++) {
	  if (FD_ISSET (fd, &sessions)) {
	    len = write(fd,buf,n);
	    if (len < n) {
	      printf ("partial write (%d of %d bytes) to session %d\n",len,n,fd);
	    } else {
	      printf ("wrote %d bytes to session %d\n",len,fd);
	    }
	  }
	}	    
      } else if (b[term].left == 0) 
	// no delimiter, buffer full, so double size
	if (resize_cbuff(b+term,b[term].len * 2) < 0)
	  printf ("resize_cbuff tty failed\n");
    }
  }

  // never get here

  return 0;
}
      
int validate_terminal (char* termstr,char* baudstr)
{
  if (!termstr) {
    printf("no tty specified\n");
    return -1;
  }
  
  printf("tty = %s\n",termstr);
  
  struct stat ttystat;
  
  if (stat(termstr, &ttystat) != 0) {
    printf("stat of tty %s failed\n",termstr);
    return -2;
  }

  if (S_ISCHR(ttystat.st_mode) == 0) {
    printf("tty %s isn't a character special device\n",termstr);
    return -3;
  }

  int fd = open(termstr, O_RDWR|O_NOCTTY|O_NDELAY);
  if (fd < 0) {
    printf("could not open device %s\n",termstr);
    return -4;
  }

  if (!isatty(fd)) {
    printf ("fd %d is not a tty\n",fd);
    return -5;
  }

  if ((tcgetattr(fd, &save) == -1) || // stash away for later restoration
      (tcgetattr(fd, &tp) == -1)) { 
    printf("failed to read attributes from %s\n",termstr);
    close(fd);
    return -6;
  }

  if (!baudstr) {
    printf("no baud rate specified\n");
    close(fd);
    return -7;
  }

  printf("baud string = %s\n",baudstr);

  int baud=atoi(baudstr);
  speed_t rate;

  switch (baud) {
  case 0: rate = B0; break;
  case 50: rate = B50; break;
  case 75: rate = B75; break;
  case 110: rate = B110; break;
  case 134: rate = B134; break;
  case 150: rate = B150; break;
  case 200: rate = B200; break;
  case 300: rate = B300; break;
  case 600: rate = B600; break;
  case 1200: rate = B1200; break;
  case 1800: rate = B1800; break;
  case 2400: rate = B2400; break;
  case 4800: rate = B4800; break;
  case 9600: rate = B9600; break;
  case 19200: rate = B19200; break;
  case 38400: rate = B38400; break;
  case 57600: rate = B57600; break;
  case 115200: rate = B115200; break;
  case 230400: rate = B230400; break;
  case 460800: rate = B460800; break;
  case 500000: rate = B500000; break;
  case 576000: rate = B576000; break;
  case 921600: rate = B921600; break;
  case 1000000: rate = B1000000; break;
  case 1152000: rate = B1152000; break;
  case 1500000: rate = B1500000; break;
  case 2000000: rate = B2000000; break;
  case 2500000: rate = B2500000; break;
  case 3000000: rate = B3000000; break;
  case 3500000: rate = B3500000; break;
  case 4000000: rate = B4000000; break;
  default:
    printf("invalid baud rate %s\n",baudstr);
    close(fd);
    return -8;
  }

  if((cfsetispeed(&tp,rate) == -1) || 
     (cfsetospeed(&tp,rate) == -1)) {
    printf("failed to set tty speed to %d\n", baud);
    close(fd);
    return -9;
  }

  return fd;
}

int restore_tty(int fd)
{
  tcsetattr(fd, TCSAFLUSH, &save);
  close(fd);
  return 0;
}

int validate_port (char* portstr) 
{
  if (!portstr) {
    printf("port string %s empty\n",portstr);
    return -1;
  }

  int port = atoi (portstr);

  if (port <= 0) {
    printf("port string %s is not an valid port number %d\n", portstr, port);
    return -2;
  }

  int optval;
  struct addrinfo hints, *result, *rp;

  memset(&hints, 0, sizeof(struct addrinfo));
  hints.ai_canonname = NULL;
  hints.ai_addr = NULL;
  hints.ai_next = NULL;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_family = AF_UNSPEC;
  hints.ai_flags = AI_PASSIVE | AI_NUMERICSERV;
  
  if (getaddrinfo(NULL, portstr, &hints, &result) != 0) {
    printf ("getaddrinfo failed\n");
    return -3;
  }
  
  int fd = -1;

  for (rp = result ; rp != NULL ; rp = rp->ai_next) {
    fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
    if (fd == -1)
      continue;

    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) == -1) {
      printf ("setsockopt failed\n");
      return -4;
    }

    if (bind(fd, rp->ai_addr, rp->ai_addrlen) == 0)
      break; // success

    // try next

    close(fd);
  }

  if (rp == NULL) {
    printf ("bind failed on all addresses\n");
    return -5;
  }

#define QUEUE_LEN 50

  if (listen (fd, QUEUE_LEN) == -1) {
    printf ("listen failed\n");
    return -6;
  }

  freeaddrinfo (result);

  return fd;
}
