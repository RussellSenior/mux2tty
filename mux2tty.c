#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <getopt.h>
#include <string.h>
#include <argp.h>
#include <errno.h>

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
#include <syslog.h>
#include <libgen.h>
#include <signal.h>

#include "cbuff.h"

const char *argp_program_version = "mux2tty 0.1";
const char *argp_program_bug_address = "mux2tty-bugs@klickitat.com";

static char doc[] =
  "mux2tty opens a tty and listens on a TCP port for connections\v\
Data from TCP connections are sent to the tty.  Data from the tty are sent to all \
TCP connections.  By default, data are line-buffered and input from connections are \
is round-robined.  Turn off round-robining using --fifo.  Turn off buffering \
with --delimiter with no argument.  Buffer on something other than lines using the \
--delimiter option with a string argument";


#define DEFAULT_DEBUG_LEVEL  0xffffffff

int verbose = 0;
int quiet = 0;
unsigned long debug = 0;
int nofork = 0;
int hardware_flowctrl = 0;

#define LINE_BUFFERING  1
#define TIU_BUFFERING   2

#define CBUFFSIZE      64

int buffering = LINE_BUFFERING;
char delim = '\n';

int tty = 0;

char* ttystr = NULL;
char* baudstr = "57600";
char* portstr = "4660";

struct termios tp, save;

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
int restore_tty(int fd);

static int
parse_opt (int key, char *arg, struct argp_state *state)
{
  int *arg_count = state->input;
  switch (key)
    {
    case 'd':
      if (!arg)
	debug = DEFAULT_DEBUG_LEVEL;
      else {
	errno = 0;
	debug = strtoul (arg, NULL, 0);
	if (errno)
	  argp_usage (state);
      }
      break;

    case 'n':
      nofork = 1;
      break;

    case 'b':
      baudstr = arg;
      break;

    case 'p':
      portstr = arg;
      break;

    case 'f':
      hardware_flowctrl = 1;
      break;

    case 'l':
      buffering = LINE_BUFFERING;
      delim = '\n';
      break;

    case 't':
      buffering = TIU_BUFFERING;
      delim = 0x4d;
      break;

    case 'v':
      verbose = 1;
      break;

    case 'q':
      quiet = 1;
      break;

    case ARGP_KEY_ARG:
      (*arg_count)++;
      switch (*arg_count) {
      case 1:
	ttystr = arg;
	break;
      case 2:
	baudstr = arg;
	break;
      case 3:
	portstr = arg;
	break;
      }
      break;

    case ARGP_KEY_END:
      if (*arg_count < 1 || *arg_count > 3)
	argp_usage (state);
      break;
    }
  return 0;
}

static void
term_handler(int sig)
{
  syslog (LOG_INFO, "captured sigint, exiting");
  restore_tty(tty);
  exit(0);
}

static void 
remove_pid_file_on_exit(int status, void *arg)
{
  char *pidfn = (char *) arg;
  if (pidfn) {
    syslog (LOG_INFO, "removing pid file %s",pidfn);
    unlink (pidfn);
  }
  return;
}

int main(int argc,char** argv)
{
  int c;

  struct argp_option options[] = {
    { "debug", 'd', "NUM", OPTION_ARG_OPTIONAL, "Turn on debugging [level]" },
    { "nofork", 'n', 0, 0, "Don't fork or daemonize" },
    { 0, 0, 0, 0, "Informational options:", -1 },
    { "verbose", 'v', 0, 0, "Be more verbose" },
    { "quiet", 'q', 0, 0, "Be quiet" },
    { 0, 0, 0, 0, "Connection parameters:", 7},
    { "baud", 'b', "<baud>", 0, "Baud for tty" },
    { "flowctrl", 'f', 0, 0, "Enable hardware flow control" },
    { "port", 'p', "<port>", 0, "Port number to listen on" },
    { 0, 0, 0, 0, "Buffering options:", 8 },
    { "line-buffering", 'l', 0, 0, "Line buffering" },
    { "tiu-buffering", 't', 0, 0, "TIU buffering" },
    { 0 }
  };

  struct argp argp = { options, parse_opt, "<tty> [<baud> [<port>]]", doc };

  struct sigaction sa;

  int arg_count = 0;
  if (argp_parse (&argp, argc, argv, 0, 0, &arg_count))
    return -1;

  if (!nofork) {
    // daemonize
    switch (fork())
      {
      case -1: return -1;
      case 0: break;
      default: _exit(EXIT_SUCCESS);
      }

    if (setsid() == -1)
      return -1;

    switch (fork())
      {
      case -1: return -1;
      case 0: break;
      default: _exit(EXIT_SUCCESS);
      }

    close (STDIN_FILENO);
    int fd = open("/dev/null", O_RDWR);

    if (fd != STDIN_FILENO)
      return -1;
    if (dup2(STDIN_FILENO, STDOUT_FILENO) != STDOUT_FILENO)
      return -1;
    if (dup2(STDIN_FILENO, STDERR_FILENO) != STDERR_FILENO)
      return -1;

    openlog ("mux2tty", LOG_PID, LOG_DAEMON);
  } else {
    openlog ("mux2tty", LOG_PID | LOG_PERROR, LOG_DAEMON);
  }

  tty = validate_terminal (ttystr, baudstr);

  if (tty < 0) {
    syslog (LOG_ERR, "opening terminal %s at %s failed with error %d", ttystr, baudstr, tty);
    return -2;
  }

  if (!nofork) {
    int len;
    char buf[64];
    char *pidfn = NULL;

    len = snprintf(buf,64,"/var/run/mux2tty.%s.pid",basename(ttystr));

    pidfn = strndup(buf,64);

    int fd = open (buf, O_CREAT | O_RDWR, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
    if (fd < 0) {
      syslog (LOG_ERR, "%m can't open pid file: %s", buf);
      return -3;
    }

    len = snprintf (buf, 64, "%d", getpid());

    if (write (fd, buf, len) != len) {
      syslog (LOG_ERR, "writing pid %s failed", buf);
      return -4;
    }

    on_exit(&remove_pid_file_on_exit,pidfn);

    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sa.sa_handler = term_handler;
    if (sigaction(SIGTERM, &sa, NULL) == -1) {
      syslog (LOG_ERR, "sigaction failure");
      return -5;
    }

    close(fd);
  }

  int port = validate_port(portstr);

  if (port < 0) {
    return -6;
  }

  if (verbose) {
    syslog (LOG_INFO, "terminal = %s ; tty fd = %d ; baud = %s", ttystr, tty, baudstr);
    syslog (LOG_INFO, "port = %s ; port number = %d",portstr,port);
  }

  struct cbuff* b = NULL;

  int len = 0;
  int nfds = 0;

  b = (struct cbuff*) calloc (tty + 1, sizeof(struct cbuff));
  if (!b) {
    syslog (LOG_ERR, "failed to allocated cbuff array for tty");
    return -7;
  }

  if (new_cbuff(b+tty,CBUFFSIZE) < 0) {
    syslog (LOG_ERR, "failed to allocated cbuff buffer for tty");
    return -8;
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
      syslog (LOG_INFO, "tty: %d ; listening %d",tty,port);
    }
    fd_set readfds,writefds;
    memcpy(&readfds,&sessions,sizeof(fd_set));
    FD_ZERO(&writefds); // clear output
    FD_SET(tty,&readfds); // tty
    FD_SET(port,&readfds); // listening for connections

    if (pending) {
      syslog (LOG_DEBUG, "writes to tty pending");
      FD_SET(tty,&writefds);
    }

    for (int fd=0 ; fd<nfds ; fd++) {
      if (FD_ISSET(fd, &sessions)) {
	syslog (LOG_DEBUG, "session %d",fd);
	int n = (buffering == LINE_BUFFERING) ?
	  cbuf_find(b+fd,delim) :
	  cbuf_findtiu(b+fd);
	if (FD_ISSET (fd, &closed)) {
	  syslog (LOG_DEBUG, "session %d is closed, don't read",fd);
	  // session is closed, so don't read
	  FD_CLR(fd,&readfds);
	  if (!n) {
	    // closed session has no more complete records
	    // and won't be getting any new ones, so release
	    // and remove from future consideration
	    syslog (LOG_DEBUG, "no complete records in closed session %d",fd);
	    free_cbuff(b+fd);
	    FD_CLR(fd,&sessions);
	    FD_CLR(fd,&closed);
	    syslog (LOG_DEBUG, "freeing cbuff and removing %d from sessions and closed lists",fd);
	  }
	}
	if (n) {
	  syslog (LOG_DEBUG, "session %d has %d bytes to write, checking tty for writability",fd,n);
	  FD_SET(tty,&writefds);
	} else if (b[fd].left == 0) {
	  syslog (LOG_DEBUG, "cbuff for session %d does not have a complete record, and is out of space",fd);
	  // no delimiter, buffer full, so double size
	  if (resize_cbuff(b+fd,b[fd].len ? b[fd].len * 2 : CBUFFSIZE) < 0)
	    syslog (LOG_DEBUG, "resize_cbuff session %d failed",fd);
	}
      }
    }

    nfds = max_fds2(&readfds,&sessions,maxfd);

    b = (struct cbuff *) realloc (b,nfds * sizeof(struct cbuff));

    int ready = select(nfds,&readfds,&writefds,NULL,NULL);

    if (verbose) 
      syslog (LOG_DEBUG, "%d fd ready",ready);

    if (ready <= 0) {
      syslog (LOG_DEBUG, "nothing readable yet, select returned %d",ready);
    } else {
      for (int fd = 0 ; fd < nfds ; fd++) {
	if (FD_ISSET (fd, &readfds)) {    
	  if (verbose) {
	    syslog (LOG_DEBUG, "read fd = %d",fd);
	  }
	  if (fd == tty) {
	    // data has arrived on tty, read into buffer
	    len = read2cbuf(b+tty,tty);
	    if (verbose) {
	      syslog (LOG_DEBUG, "read %d bytes from tty",len);
	    }
	    if (len < 0) {
	      // error reading tty
	      syslog (LOG_DEBUG, "error reading tty");
	    } else if (len == 0) {
	      // tty has closed, exit
	      for (int i=0 ; i<nfds ; i++) {
		if (FD_ISSET (i, &sessions) && !FD_ISSET (i, &closed)) {
		  close(i);
		  syslog (LOG_DEBUG, "closed session %d",i);
		  FD_SET (i, &closed);
		}
	      }
	      close(port);
	      if (verbose)
		syslog (LOG_DEBUG, "tty closed, exiting");
	      return 0;
	    } 
	  } else if (fd == port) {
	    // connection request on listening port
	    struct sockaddr_storage naddr;
	    int addrlen = sizeof(naddr);
	    char hostname[NI_MAXHOST];
	    char service[NI_MAXSERV];

	    if (verbose) {
	      syslog (LOG_INFO, "accepting connection on port %d",port);
	    }

	    int nfd = accept (port, (struct sockaddr *) &naddr, &addrlen);

	    if (nfd < 0) {
	      syslog (LOG_ERR, "error %d accepting connection on port %d",nfd,port);
	    } else {
	      FD_SET (nfd, &sessions);
	      if (nfd >= maxfd) 
		maxfd = nfd + 1;

	      nfds = max_fds(&sessions,maxfd);

	      b = (struct cbuff *) realloc (b, nfds * sizeof(struct cbuff));
	      if (!b) {
		syslog (LOG_ERR, "failure to allocate cbuff array for %d",nfd);
		return -5;
	      }

	      if (new_cbuff(b+nfd,CBUFFSIZE) < 0) {
		syslog (LOG_ERR, "failed to allocated cbuff buffer for %d",nfd);
		return -6;
	      }

	      if (getnameinfo((struct sockaddr *) &naddr,addrlen,
			      hostname,NI_MAXHOST,
			      service,NI_MAXSERV,
			      NI_NUMERICHOST | NI_NUMERICSERV) != 0) {
		syslog (LOG_ERR, "getnameinfo failed");
	      } else {
		syslog (LOG_INFO, "connection %d from %s:%s",nfd,hostname,service);
	      }
	    }
	  } else {
	    // received data from a session
	    len = read2cbuf (b+fd,fd);
	    if (verbose) {
	      syslog (LOG_DEBUG, "read %d bytes from session %d",len,fd);
	    }
	    if (len < 0) {
	      // error reading session
	      syslog (LOG_ERR, "error reading fd %d",fd);
	    } else if (len == 0) {
	      // session closed
	      syslog (LOG_DEBUG, "closing session %d cbuff contains %d bytes",
		      fd, b[fd].len - b[fd].left);
	      close(fd);
	      syslog (LOG_DEBUG, "marking session %d closed",fd);
	      FD_SET (fd, &closed);
	    } 
	  }
	}
      }
      // try to write
      if (FD_ISSET (tty, &writefds)) {
	syslog (LOG_DEBUG, "tty is writable");
	if (pending) {
	  syslog (LOG_DEBUG, "serving pending buffer %d",pending);
	  int n = (buffering == LINE_BUFFERING) ?
	    cbuf_find (b+pending,delim) :
	    cbuf_findtiu(b+pending);
	  int len = cbuf2write(b+pending,tty,n);
	  if (len == n) {
	    syslog (LOG_DEBUG, "completed pending buffer %d",pending);
	    pending = 0;
	  }
	}
	syslog (LOG_DEBUG, "pending = %d",pending);
	if (!pending) {
	  for (int i=0 ; i<nfds ; i++) {
	    int fd = (last + i + 1) % nfds;
	    if (FD_ISSET (fd, &sessions)) {
	      syslog (LOG_DEBUG, "looking for delimiter in session %d buffer",fd);
	      int n = (buffering == LINE_BUFFERING) ?
		cbuf_find (b+fd,delim) :
		cbuf_findtiu (b+fd);
	      if (n) {
		syslog (LOG_DEBUG, "record delimter found at offset %d of buffer %d",n,fd);
		int len = cbuf2write(b+fd,tty,n);
		syslog (LOG_DEBUG, "wrote %d bytes to tty from session %d",len,fd);
		if (len > 0 && len < n) {
		  pending = fd;
		} 
		last = fd;
		syslog (LOG_DEBUG, "last session %d",last);
	      } else if (b[fd].left == 0) {
		// no delimiter, buffer full, so double size
		syslog (LOG_DEBUG, "resizing buffer for session %d",fd);
		if (resize_cbuff(b+fd,b[fd].len ? b[fd].len * 2 : CBUFFSIZE) < 0)
		  syslog (LOG_DEBUG, "resize_cbuff session %d failed",fd);
	      }
	    }
	  }
	}
      }
      // check tty cbuff for records, if ready, send to sessions
      syslog (LOG_DEBUG, "looking for delimiter in tty %d buffer",tty);
      int n = (buffering == LINE_BUFFERING) ?
	cbuf_find (b+tty,delim) :
	cbuf_finduit (b+tty);
      if (n) {
	char buf[CBUFFSIZE];
	int len = cbuf2buf (b+tty,buf,n);
	syslog (LOG_DEBUG, "copied %d of %d chars to buffer",len,n);
	for (int fd=0 ; fd<nfds ; fd++) {
	  if (FD_ISSET (fd, &sessions)) {
	    len = write(fd,buf,n);
	    if (len < n) {
	      syslog (LOG_DEBUG, "partial write (%d of %d bytes) to session %d",len,n,fd);
	    } else {
	      syslog (LOG_DEBUG, "wrote %d bytes to session %d",len,fd);
	    }
	  }
	}	    
      } else if (b[tty].left == 0)
	// no delimiter, buffer full, so double size
	if (resize_cbuff(b+tty,b[tty].len ? b[tty].len * 2 : CBUFFSIZE) < 0)
	  syslog (LOG_DEBUG, "resize_cbuff tty failed");
    }
  }

  // never get here

  return 0;
}
      
int validate_terminal (char* ttystr,char* baudstr)
{
  if (!ttystr) {
    syslog (LOG_ERR, "no tty specified");
    return -1;
  }
  
  syslog (LOG_DEBUG, "tty = %s",ttystr);
  
  struct stat ttystat;
  
  if (stat(ttystr, &ttystat) != 0) {
    syslog (LOG_ERR, "stat of tty %s failed",ttystr);
    return -2;
  }

  if (S_ISCHR(ttystat.st_mode) == 0) {
    syslog (LOG_ERR, "tty %s isn't a character special device",ttystr);
    return -3;
  }

  int fd = open(ttystr, O_RDWR|O_NOCTTY|O_NDELAY);
  if (fd < 0) {
    syslog (LOG_ERR, "could not open device %s",ttystr);
    return -4;
  }

  if (!isatty(fd)) {
    syslog (LOG_ERR, "fd %d is not a tty",fd);
    return -5;
  }

  if ((tcgetattr(fd, &save) == -1) || // stash away for later restoration
      (tcgetattr(fd, &tp) == -1)) { 
    syslog (LOG_ERR, "failed to read attributes from %s",ttystr);
    close(fd);
    return -6;
  }

  if (!baudstr) {
    syslog (LOG_ERR, "no baud rate specified");
    close(fd);
    return -7;
  }

  syslog (LOG_DEBUG, "baud string = %s",baudstr);

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
    syslog (LOG_ERR, "invalid baud rate %s",baudstr);
    close(fd);
    return -8;
  }

  if((cfsetispeed(&tp,rate) == -1) || 
     (cfsetospeed(&tp,rate) == -1)) {
    syslog (LOG_ERR, "failed to set tty speed to %d", baud);
    close(fd);
    return -9;
  }

  tp.c_lflag &= ~(ICANON | ISIG | IEXTEN | ECHO);
  tp.c_iflag &= ~(BRKINT | ICRNL | IGNBRK | IGNCR | INLCR | INPCK | ISTRIP | IXON | PARMRK);
  tp.c_oflag &= ~OPOST;
  if (hardware_flowctrl) 
    tp.c_cflag |= CRTSCTS; // enable hardware flow control
  tp.c_cflag &= ~(CSTOPB | PARENB | CSIZE); // clear 2-stop-bits, parity, and character size mask
  tp.c_cflag |= CS8; // set 8-bit characters
  tp.c_cc[VMIN] = 1;
  tp.c_cc[VTIME] = 0;

  if (tcsetattr(fd, TCSAFLUSH, &tp) == -1) {
    syslog (LOG_ERR, "failed to set raw mode");
    return -10;
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
    syslog (LOG_ERR, "port string %s empty",portstr);
    return -1;
  }

  int port = atoi (portstr);

  if (port <= 0) {
    syslog (LOG_ERR, "port string %s is not an valid port number %d", portstr, port);
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
    syslog (LOG_ERR, "getaddrinfo failed");
    return -3;
  }
  
  int fd = -1;

  for (rp = result ; rp != NULL ; rp = rp->ai_next) {
    fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
    if (fd == -1)
      continue;

    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) == -1) {
      syslog (LOG_ERR, "setsockopt failed");
      return -4;
    }

    if (bind(fd, rp->ai_addr, rp->ai_addrlen) == 0)
      break; // success

    // try next

    close(fd);
  }

  if (rp == NULL) {
    syslog (LOG_ERR, "bind failed on all addresses");
    return -5;
  }

#define QUEUE_LEN 50

  if (listen (fd, QUEUE_LEN) == -1) {
    syslog (LOG_ERR, "listen failed");
    return -6;
  }

  freeaddrinfo (result);

  return fd;
}
