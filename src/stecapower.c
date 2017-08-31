/* Steca500 power reading
   Martin van Es 2017 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/types.h> 
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <wiringPi.h>

/* daemon settings */
char runningdir[] = "/tmp";
char lockfilename[] = "stecapower.lck";
char logfilename[] = "stecapower.log";
char statefilename[] = "stecapower.state";
FILE *statefile;
char state[8];
struct itimerval tmr;

/* server settings */
int listenport = 54321;

/* Power vars */
unsigned int globalCounter, globalCounterLast;
struct timeval tv_now;
double last, now, tdelta, power;

void log_message(char *filename, char *message) {
  FILE *logfile;
  logfile=fopen(filename,"a");
  if(!logfile) return;
  fprintf(logfile,"%s\n",message);
  fclose(logfile);
}

/* stecapower cares about SIGHUP and SIGTERM.
   SIGHUP will reset globalCounter so that we only measure day totals
   SIGTERM will save globalCounter to file so we don't loose day totals on restart
*/
void signal_handler(int sig) {
  switch(sig) {
  case SIGHUP:
    log_message(logfilename,"Caught hangup signal");
    globalCounter = globalCounterLast = 0;
    break;
  case SIGTERM:
    log_message(logfilename,"Caught terminate signal");
    statefile = fopen(statefilename,"w");
    if(statefile) {
      fprintf(statefile,"%d\n", globalCounter);
      fclose(statefile);
    }
    exit(0);
    break;
  }
}

/* Calculate power based on current FC frequency derived from time between last two pulses */
void timer_handler(int sig) {
  //log_message(logfilename, "Timer expired");
  if (globalCounter - globalCounterLast > 2) power = 10.0/tdelta;
  else power = 0;
  globalCounterLast = globalCounter;
}

/* Calculate tdelta since last pulse, increase globalCounter by one */
void interrupt_handler(void) {
  gettimeofday(&tv_now, NULL);
  now = tv_now.tv_sec + (1.0/1000000) * tv_now.tv_usec;
  tdelta = now - last;
  last = now;
  globalCounter++;
}

/* Fork ourselves to background and register power calculation timer */
void daemonize() {
  int i,lfp;
  char str[10];
  
  if (getppid() == 1) return; /* already a daemon */

  i = fork();
  if (i<0) exit(1); /* fork error */
  if (i>0) exit(0); /* parent exits */

  /* child (daemon) continues */
  setsid(); /* obtain a new process group */

  for (i = getdtablesize(); i>=0; --i) close(i); /* close all descriptors */
  i = open("/dev/null", O_RDWR); dup(i); dup(i); /* handle standard I/O */

  chdir(runningdir); /* change running directory */
  lfp = open(lockfilename, O_RDWR|O_CREAT, 0640);
  if (lfp < 0) exit(1); /* can not open */
  if (lockf(lfp, F_TLOCK, 0) < 0) exit(0); /* can not lock */

  /* first instance continues */
  sprintf(str,"%d\n",getpid());
  write(lfp, str, strlen(str)); /* record pid to lockfile */

  /* register signals */
  signal(SIGCHLD, SIG_IGN); /* ignore child */
  signal(SIGTSTP, SIG_IGN); /* ignore tty signals */
  signal(SIGTTOU, SIG_IGN);
  signal(SIGTTIN, SIG_IGN);
  signal(SIGHUP, signal_handler); /* catch hangup signal */
  signal(SIGTERM, signal_handler); /* catch kill signal */

  /* setup Timer */
  tmr.it_interval.tv_sec = 5;
  tmr.it_interval.tv_usec = 0;
  tmr.it_value.tv_sec = 1;
  tmr.it_value.tv_usec = 0;
  if (setitimer (ITIMER_REAL, &tmr, NULL) < 0)
      log_message(logfilename, "timer init failed");
  signal(SIGALRM, timer_handler);
}

/* Create socket to connect for readout of measurements */
int bindsocket(int *sock, int port) {
  int optval;
  size_t optlen;
  struct sockaddr_in svr_addr;
  
  *sock = socket(AF_INET, SOCK_STREAM, 0);
  if (sock < 0)
    return 1;

  optval = 1;
  optlen = sizeof(int);
  setsockopt(*sock, SOL_SOCKET, SO_REUSEADDR, &optval, optlen);

  svr_addr.sin_family = AF_INET;
  svr_addr.sin_addr.s_addr = INADDR_ANY;
  svr_addr.sin_port = htons(port);

  if (bind(*sock, (struct sockaddr *) &svr_addr, sizeof(svr_addr)) == -1) {
    close(*sock);
    return 1;
  }
  return 0;
}

/* The main program */
int main() {
  int result;
  int client_fd;
  int sock;
  struct sockaddr_in cli_addr;
  socklen_t sin_len = sizeof(cli_addr);
  char response[40];
  int rlen;

  /* Make a daemon */
  daemonize();
  log_message(logfilename, "daemonized");

  /* Setup socket */
  if ((result = bindsocket(&sock, listenport)) > 0) {
    log_message(logfilename, "Can't bind");
    exit(result);
  } else {
    log_message(logfilename, "bound");
  }
  listen(sock, 5);

  /* Setup Raspberry GPIO */
  wiringPiSetup () ;
  pinMode(0, INPUT);
  pullUpDnControl(0, PUD_DOWN);

  /* Setup interrupt handler */
  if (wiringPiISR (0, INT_EDGE_FALLING, &interrupt_handler) < 0)
    log_message(logfilename, "Unable to setup ISR");
  
  /* Setup static vars */
  globalCounter = globalCounterLast = 0;
  tdelta = now = last = power =0;

  /* Read pulsecounter from file if available */
  statefile = fopen(statefilename,"r");
  if (statefile) {
    log_message(logfilename, "Statefile found");
    fgets(state, 8, statefile);
    globalCounter = atoi(state);
    fclose(statefile);
    unlink(statefilename);
  } else {
   log_message(logfilename, "No statefile found");
  }
 
  /* Main loop */ 
  while (1) {
    client_fd = accept(sock, (struct sockaddr *) &cli_addr, &sin_len);
    //log_message(logfilename, "Connection accepted");

    if (client_fd == -1) {
      log_message(logfilename, "Can't accept");
      continue;
    }

    rlen = sprintf (response, "%.0f %.0f %d\n", power, (1.0/360) * globalCounter, globalCounter);
    write(client_fd, response, rlen);
    close(client_fd);
  }
}
