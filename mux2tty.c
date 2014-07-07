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

int max_fds(fd_set *set) 
{
  int max = FD_SETSIZE;
  while (max > 0 && !FD_ISSET(max-1, set))
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
    printf("opening terminal %s/%s failed with error %d\n",termstr,baudstr,term);
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

#define BUF_SIZE  10240

  char buf[BUF_SIZE];
  int len = 0;
  int nfds = 0;

  fd_set sessions;
  FD_ZERO(&sessions);

  while (1) {

    if (verbose) {
      printf ("tty: %d ; listening %d\n",term,port);
    }
    fd_set readfds;
    memcpy(&readfds,&sessions,sizeof(fd_set));
    FD_SET(term,&readfds); // tty
    FD_SET(port,&readfds); // listening for connections

    nfds = max_fds(&readfds);

    int ready = select(nfds,&readfds,NULL,NULL,NULL);

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
	    // serial data has arrived, send it to all sessions
	    len = read (term, buf, sizeof(buf));
	    if (verbose) {
	      printf ("read %d bytes: %s\n",len,buf);
	    }
	    if (len < 0) {
	      // error reading tty
	      printf ("error reading tty\n");
	    } else if (len == 0) {
	      // tty has closed, exit
	      for (int i=0 ; i<nfds ; i++) {
		if (FD_ISSET (i, &sessions)) {
		  close(i);
		  FD_CLR (i, &sessions);
		}
	      }
	      close(port);
	      if (verbose)
		printf ("tty closed, exiting\n");
	      return 0;
	    } else {
	      // got bytes from tty, echo onto all open sessions
	      for (int i=0 ; i<nfds ; i++) {
		if (FD_ISSET (i, &sessions)) {
		  for(int l=0 ; l<len ; ) {
		    int result = write (i, buf+l, len-l);
		    if (result > 0)
		      l += result;
		    else
		      printf("write failure of %d bytes to fd %d, retrying\n",len-l,i);
		  }
		}
	      }
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
	      nfds = max_fds(&sessions);

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
	    // received data from a session, send to tty
	    len = read (fd, buf, sizeof(buf));
	    if (len < 0) {
	      // error reading session
	      printf ("error reading fd %d\n",fd);
	    } else if (len == 0) {
	      // session closed
	      FD_CLR (fd, &sessions);
	      nfds = max_fds(&sessions);
	      if (verbose)
		printf ("closing session %d\n",fd);
	      close(fd);
	    } else {
	      if (verbose)
		printf ("writing %s to tty\n",buf);
	      for(int l=0 ; l<len ; ) {
		int result = write (term, buf+l, len-l);
		if (result > 0)
		  l += result;
		else {
		  printf("write failure of %d bytes to fd %d, retrying\n",len-l,term);
		  struct timespec nap;
		  nap.tv_sec = 0;
		  nap.tv_nsec = 1000000;
		  nanosleep(&nap,NULL);
		}
	      }
	    }
	  }
	}
      }
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
