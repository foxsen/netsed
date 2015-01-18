//  This work is based on the original netsed 0.01c by Michal Zalewski.
//  Please contact Julien VdG <julien@silicone.homelinux.org> if you encounter
//  any problems with this version.
//  The changes compared to version 0.01c are related in the NEWS file.

///@mainpage
///
/// This documentation is targeting netsed developers, if you are a user
/// either launch netsed without parameters or read the README file
/// (@link README @endlink).
///
///@par
/// - Currently netsed is implemented in a single file: netsed.c
/// - some TODOs are gathered on the @link todo @endlink page,
///   some others are in the TODO file.
/// .

///@file netsed.c
///@brief netsed is implemented in this single file.
///@par Architecture
/// Netsed is implemented as a select socket dispatcher.
/// First a main socket server is created (#lsock), each connection to this
/// socket create a context stored in the tracker_s structure and added to
/// the #connections list.
/// Each connection has
/// - a connected socket (tracker_s::csock) returned by the accept() function
///   for tcp, or
/// - a connection socket address (tracker_s::csa) filled by recvfrom() for udp.
/// - a dedicated forwarding socket (tracker_s::fsock) connected to the server.
/// .
/// All sockets are added to the select() call and managed by the dispatcher
/// as follows:
/// - When packets are received from the client, the rules are applied by
///   sed_the_buffer() and the packet is send to the server.
///   This is the role of client2server_sed() function. It is only used for tcp.
/// - When packets are received from the server, the rules are applied by
///   sed_the_buffer() and the packet is send to the corresponding client.
///   This is the role of server2client_sed() function.
/// - For udp only, connection from client to netsed are not established
///   so netsed need to lookup existing #connections to find the corresponding
///   established link, if any. The lookup is done by comparing tracker_s::csa.
///   Once the connection is found or created, the rules are applied
///   by sed_the_buffer() and the packet is send to the server.
///   This is the role of b2server_sed() function.
/// .
/// @note For tcp tracker_s::csa is NULL and for udp the tracker_s::csock is
/// filled with #lsock. This is done in order to share code and avoid
/// discriminating between tcp or udp everywhere, sendto are done on
/// tracker_s::csock with tracker_s::csa only and the actual value of those
/// will reflect the needs.
///
/// @note I'm saying packets and connections, but for udp these are actually
/// datagrams and pseudo-connections. The pseudo-connection is defined by the
/// fact that the client uses the same address and port (same tracker_s::csa)
/// with a life time defined by #UDP_TIMEOUT to clean the connection list.
///
/// @todo Implements features listed in TODO file.

///@page README User documentation
/// The README file:
///@verbinclude README

///@page todo The TODO list
/// The TODO file:
///@verbinclude TODO

#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>
#include <stdlib.h>
#include <signal.h>
#include <netdb.h>
#include <time.h>

#if ANDROID
#define in_port_t int
#endif
/// Define for transparent proxy with linux netfilter.
/// Else use getsockname() supposing the socket receive the original
/// destination information directly.
#if __linux__
#define LINUX_NETFILTER
#endif

#ifdef LINUX_NETFILTER
#include <limits.h>
#include <linux/netfilter_ipv4.h>
#endif

/// Current version (recovered by Makefile for several release checks)
#define VERSION "1.00b"
/// max size for buffers
#define MAX_BUF  100000

/// printf to stderr
#define ERR(x...) fprintf(stderr,x)

// Uncomment to add a lot of debug information.
//#define DEBUG
#ifdef DEBUG
/// printf for debug information
#define DBG(x...) printf(x)
#else
/// Disabled debug prints.
#define DBG(x...)
#endif

/// Timeout for udp 'connections' in seconds
#define UDP_TIMEOUT 30

/// Rule item.
struct rule_s {
  /// binary buffer to match.
  char *from;
  /// binary buffer replacement.
  char *to;
  /// match from the command line.
  const char *forig;
  /// replacement from the command line.
  const char *torig;
  /// length of #from buffer.
  int fs;
  /// length of #to buffer.
  int ts;
};

/// Connection state
enum state_e {
  /// udp datagram received by netsed and send to server, no response yet.
  UNREPLIED,
  /// tcp accepted connection or udp 'connection' with a response from server.
  ESTABLISHED,
  /// tcp or udp disconnected (detected by an error on read or send).
  /// @note all values after and including #DISCONNECTED are considered as
  /// error and the connection will be discarded.
  DISCONNECTED,
  /// udp timeout expired.
  TIMEOUT
};

/// This structure is used to track information about open connections.
struct tracker_s {
  /// recvfrom information: 'connect' address for udp
  struct sockaddr* csa;
  /// size of #csa
  socklen_t csl;
  /// Connection socket to client
  int csock;
  /// Socket to forward to server
  int fsock;
  /// Last event time, for udp timeout
  time_t time;
  /// Connection state
  enum state_e state;
  /// By connection TTL
  int* live;

  /// chain it !
  struct tracker_s * n;
};

/// Store current time (just after select returned).
time_t now;
/// Listening socket.
int lsock;
/// Number of rules.
int rules;
/// Array of all rules.
struct rule_s *rule;
/// TTL part of the rule as a flat array to be able to copy it
/// in tracker_s::live for each connections.
int *rule_live;

/// List of connections.
struct tracker_s * connections = NULL;

/// True when SIGINT signal was received.
volatile int stop=0;

/// Display an error message followed by usage information.
/// @param why the error message.
void usage_hints(const char* why) {
  ERR("Error: %s\n\n",why);
  ERR("Usage: netsed proto lport rhost rport rule1 [ rule2 ... ]\n\n");
  ERR("  proto   - protocol specification (tcp or udp)\n");
  ERR("  lport   - local port to listen on (see README for transparent\n");
  ERR("            traffic intercepting on some systems)\n");
  ERR("  rhost   - where connection should be forwarded (0 = use destination\n");
  ERR("            address of incoming connection, see README)\n");
  ERR("  rport   - destination port (0 = dst port of incoming connection)\n");
  ERR("  ruleN   - replacement rules (see below)\n\n");
  ERR("General syntax of replacement rules: s/pat1/pat2[/expire]\n\n");
  ERR("This will replace all occurrences of pat1 with pat2 in any matching packet.\n");
  ERR("An additional parameter (count) can be used to expire a rule after 'count'\n");
  ERR("successful substitutions for a given connection. Eight-bit characters,\n");
  ERR("including NULL and '/', can be passed using HTTP-like hex escape\n");
  ERR("sequences (e.g. CRLF as %%0a%%0d).\n");
  ERR("A match on '%%' can be achieved by specifying '%%%%'. Examples:\n\n");
  ERR("  's/andrew/mike/1'     - replace 'andrew' with 'mike' (only first time)\n");
  ERR("  's/andrew/mike'       - replace all occurrences of 'andrew' with 'mike'\n");
  ERR("  's/andrew/mike%%00%%00' - replace 'andrew' with 'mike\\x00\\x00'\n");
  ERR("                          (manually padding to keep original size)\n");
  ERR("  's/%%%%/%%2f/20'         - replace the 20 first occurrence of '%%' with '/'\n\n");
  ERR("Rules are not active across packet boundaries, and they are evaluated\n");
  ERR("from first to last, not yet expired rule, as stated on the command line.\n");
  exit(1);
}

/// Helper function to free a tracker_s item.
/// csa will be freed if needed, sockets will be closed
/// @param conn pointer to free.
void freetracker (struct tracker_s * conn)
{
  if(conn->csa != NULL) { // udp
    free(conn->csa);
  } else { // tcp
    close(conn->csock);
  }
  close(conn->fsock);
  free(conn);
}

/// Close all sockets
/// to use before exit.
void clean_socks(void)
{
  close(lsock);
  // close all tracker
  while(connections != NULL) {
    struct tracker_s * conn = connections;
    connections = conn->n;
    freetracker(conn);
  }
}

#ifdef __GNUC__
// avoid gcc from inlining those two function when optimizing, as otherwise
// the function would break strict-aliasing rules by dereferencing pointers...
in_port_t get_port(struct sockaddr *sa) __attribute__ ((noinline));
void set_port(struct sockaddr *sa, in_port_t port) __attribute__ ((noinline));
#endif

/// Extract the port information from a sockaddr for both IPv4 and IPv6.
/// @param sa sockaddr to get port from
in_port_t get_port(struct sockaddr *sa) {
  switch (sa->sa_family) {
    case AF_INET:
      return ntohs(((struct sockaddr_in *) sa)->sin_port);
    case AF_INET6:
      return ntohs(((struct sockaddr_in6 *) sa)->sin6_port);
    default:
      return 0;
  }
} /* get_port(struct sockaddr *) */

/// Set the port information in a sockaddr for both IPv4 and IPv6.
/// @param sa   sockaddr to update
/// @param port port value
void set_port(struct sockaddr *sa, in_port_t port) {
  switch (sa->sa_family) {
    case AF_INET:
      ((struct sockaddr_in *) sa)->sin_port = htons(port);
      break;
    case AF_INET6:
      ((struct sockaddr_in6 *) sa)->sin6_port = htons(port);
    default:
      break;
  }
} /* set_port(struct sockaddr *, in_port_t) */

/// Detect if address in the addr_any value for both IPv4 and IPv6.
/// @param sa sockaddr to test
/// @return true if sa in addr_any
int is_addr_any(struct sockaddr *sa) {
  switch (sa->sa_family) {
    case AF_INET:
      return (((struct sockaddr_in *) sa)->sin_addr.s_addr == htonl(INADDR_ANY));
    case AF_INET6:
      return !memcmp(&((struct sockaddr_in6 *) sa)->sin6_addr, &in6addr_any, sizeof(in6addr_any));
    default:
      return 0;
  }
} /* is_addr_any(struct sockaddr *) */


/// Display an error message and exit.
void error(const char* reason) {
  ERR("[-] Error: %s\n",reason);
  ERR("netsed: exiting.\n");
  clean_socks();
  exit(2);
}

/// Hex digit to parsing the % notation in rules
char hex[]="0123456789ABCDEF";

/// Convert the % notation in rules to plain binary data
/// @param r rule to update
void shrink_to_binary(struct rule_s* r) {
  unsigned int i;

  r->from=malloc(strlen(r->forig));
  r->to=malloc(strlen(r->torig));
  if ((!r->from) || (!r->to)) error("shrink_to_binary: unable to malloc() buffers");

  for (i=0;i<strlen(r->forig);i++) {
    if (r->forig[i]=='%') {
      // Have to shrink.
      i++;
      if (r->forig[i]=='%') {
        // '%%' -> '%'
        r->from[r->fs]='%';
        r->fs++;
      } else {
        int hexval;
        char* x;
        if (!r->forig[i]) error("shrink_to_binary: src pattern: unexpected end.");
        if (!r->forig[i+1]) error("shrink_to_binary: src pattern: unexpected end.");
        x=strchr(hex,toupper(r->forig[i]));
        if (!x) error("shrink_to_binary: src pattern: non-hex sequence.");
        hexval=(x-hex)*16;
        x=strchr(hex,toupper(r->forig[i+1]));
        if (!x) error("shrink_to_binary: src pattern: non-hex sequence.");
        hexval+=(x-hex);
        r->from[r->fs]=hexval;
        r->fs++; i++;
      }
    } else {
      // Plaintext case.
      r->from[r->fs]=r->forig[i];
      r->fs++;
    }
  }

  for (i=0;i<strlen(r->torig);i++) {
    if (r->torig[i]=='%') {
      // Have to shrink.
      i++;
      if (r->torig[i]=='%') {
        // '%%' -> '%'
        r->to[r->ts]='%';
        r->ts++;
      } else {
        int hexval;
        char* x;
        if (!r->torig[i]) error("shrink_to_binary: dst pattern: unexpected end.");
        if (!r->torig[i+1]) error("shrink_to_binary: dst pattern: unexpected end.");
        x=strchr(hex,toupper(r->torig[i]));
        if (!x) error("shrink_to_binary: dst pattern: non-hex sequence.");
        hexval=(x-hex)*16;
        x=strchr(hex,toupper(r->torig[i+1]));
        if (!x) error("shrink_to_binary: dst pattern: non-hex sequence.");
        hexval+=(x-hex);
        r->to[r->ts]=hexval;
        r->ts++; i++;
      }
    } else {
      // Plaintext case.
      r->to[r->ts]=r->torig[i];
      r->ts++;
    }
  }
}

/// Bind forward socket to given port.
/// @param af      address family.
/// @param tcp     1 tcp, 0 udp.
/// @param portstr string representing the port to bind
///                (will be resolved using getaddrinfo()).
void bind_forward(int fsock, int af, int tcp, const char *portstr) {
  int ret;
  struct addrinfo hints, *res, *reslist;

  memset(&hints, '\0', sizeof(hints));
  hints.ai_family = af;
  hints.ai_flags = AI_PASSIVE;
  hints.ai_socktype = tcp ? SOCK_STREAM : SOCK_DGRAM;

  if ((ret = getaddrinfo(NULL, portstr, &hints, &reslist))) {
    ERR("getaddrinfo(): %s\n", gai_strerror(ret));
    error("Impossible to resolve listening port.");
  }
  /* We have useful addresses. */
  for (res = reslist; res; res = res->ai_next) {
    int one = 1;

    setsockopt(fsock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (bind(fsock, res->ai_addr, res->ai_addrlen) < 0) {
      ERR("bind(): %s", strerror(errno));
      close(fsock);
      continue;
    }
  }
}

/// Bind and optionally listen to a socket for netsed server port.
/// @param af      address family.
/// @param tcp     1 tcp, 0 udp.
/// @param portstr string representing the port to bind
///                (will be resolved using getaddrinfo()).
void bind_and_listen(int af, int tcp, const char *portstr) {
  int ret;
  struct addrinfo hints, *res, *reslist;

  memset(&hints, '\0', sizeof(hints));
  hints.ai_family = af;
  hints.ai_flags = AI_PASSIVE;
  hints.ai_socktype = tcp ? SOCK_STREAM : SOCK_DGRAM;

  if ((ret = getaddrinfo(NULL, portstr, &hints, &reslist))) {
    ERR("getaddrinfo(): %s\n", gai_strerror(ret));
    error("Impossible to resolve listening port.");
  }
  /* We have useful addresses. */
  for (res = reslist; res; res = res->ai_next) {
    int one = 1;

    if ( (lsock = socket(res->ai_family, res->ai_socktype, res->ai_protocol)) < 0)
      continue;
    setsockopt(lsock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    //fcntl(lsock,F_SETFL,O_NONBLOCK);
    /* Make our best to decide on dual-stacked listener. */
    one = (af == 0) ? 0 /* AF_UNSPEC given */ : 1; /* Preconditioned addr */
//openwrt has not defined this
#if defined(IPV6_V6ONLY)
    if (res->ai_family == AF_INET6)
      if (setsockopt(lsock, IPPROTO_IPV6, IPV6_V6ONLY, &one, sizeof(one)))
        printf("    Failed to unset IPV6_V6ONLY: %s.\n", strerror(errno));
#endif
    if (bind(lsock, res->ai_addr, res->ai_addrlen) < 0) {
      ERR("bind(): %s", strerror(errno));
      close(lsock);
      continue;
    }
    if (tcp) {
      if (listen(lsock, 16) < 0) {
        close(lsock);
        continue;
      }
    } else { // udp
      int one=1;
      setsockopt(lsock,SOL_SOCKET,SO_OOBINLINE,&one,sizeof(int));
    }
    /* Successfully bound and now also listening. */
    break;
  }
  freeaddrinfo(reslist);
  if (res == NULL)
    error("Listening socket failed.");
}

/// Buffer for receiving a single packet or datagram
char buf[MAX_BUF];
/// Buffer containing modified packet or datagram
char b2[MAX_BUF];

/// Applies the rules to global buffer buf.
/// @param siz useful size of the data in buf.
/// @param live TTL state of current connection.
int sed_the_buffer(int siz, int* live) {
  int i=0,j=0;
  int newsize=0;
  int changes=0;
  int gotchange=0;
  for (i=0;i<siz;) {
    gotchange=0;
    for (j=0;j<rules;j++) {
      if ((!memcmp(&buf[i],rule[j].from,rule[j].fs)) && (live[j]!=0)) {
        changes++;
        gotchange=1;
        printf("    Applying rule s/%s/%s...\n",rule[j].forig,rule[j].torig);
        live[j]--;
        if (live[j]==0) printf("    (rule just expired)\n");
        memcpy(&b2[newsize],rule[j].to,rule[j].ts);
        newsize+=rule[j].ts;
        i+=rule[j].fs;
        break;
      }
    }
    if (!gotchange) {
      b2[newsize]=buf[i];
      if (isprint(buf[i])) 
        printf("%c",buf[i]);
      else 
	printf(" ");
      newsize++;
      i++;
      if (i%80 == 0) printf("\n");
    }
  }
  if (!changes) printf("[*] Forwarding untouched packet of size %d.\n",siz);
  else printf("[*] Done %d replacements, forwarding packet of size %d (orig %d).\n",
              changes,newsize,siz);
  return newsize;
}


// Prototype this function so that the content is in the same order as in
// previous read_write_sed function. (ease patch and diff)
void b2server_sed(struct tracker_s * conn, ssize_t rd);

/// Receive a packet or datagram from the server, 'sed' it, send it to the
/// client.
/// @param conn connection giving the sockets to use.
void server2client_sed(struct tracker_s * conn) {
    ssize_t rd;
    rd=read(conn->fsock,buf,sizeof(buf));
    if ((rd<0) && (errno!=EAGAIN))
    {
      DBG("[!] server disconnected. (rd err) %s\n",strerror(errno));
      conn->state = DISCONNECTED;
    }
    if (rd == 0) {
      // nothing read but select said ok, so EOF
      DBG("[!] server disconnected. (rd)\n");
      conn->state = DISCONNECTED;
    }
    if (rd>0) {
      printf("[+] Caught server -> client packet.\n");
      rd=sed_the_buffer(rd, conn->live);
      conn->time = now;
      conn->state = ESTABLISHED;
      if (sendto(conn->csock,b2,rd,0,conn->csa, conn->csl)<=0) {
        DBG("[!] client disconnected. (wr)\n");
        conn->state = DISCONNECTED;
      }
    }
}

/// Receive a packet from the client, 'sed' it, send it to the server.
/// @param conn connection giving the sockets to use.
void client2server_sed(struct tracker_s * conn) {
    ssize_t rd;
    rd=read(conn->csock,buf,sizeof(buf));
    if ((rd<0) && (errno!=EAGAIN))
    {
      DBG("[!] client disconnected. (rd err)\n");
      conn->state = DISCONNECTED;
    }
    if (rd == 0) {
      // nothing read but select said ok, so EOF
      DBG("[!] client disconnected. (rd)\n");
      conn->state = DISCONNECTED;
    }
    b2server_sed(conn, rd);
}

/// Send the content of global buffer b2 to the server as packet or datagram.
/// @param conn connection giving the sockets to use.
/// @param rd   size of b2 content.
void b2server_sed(struct tracker_s * conn, ssize_t rd) {
    if (rd>0) {
      printf("[+] Caught client -> server packet.\n");
      rd=sed_the_buffer(rd, conn->live);
      conn->time = now;
      if (write(conn->fsock,b2,rd)<=0) {
        DBG("[!] server disconnected. (wr)\n");
        conn->state = DISCONNECTED;
      }
    }
}

/// Handle SIGINT signal for clean exit.
void sig_int(int signo)
{
  DBG("[!] user interrupt request (%d)\n",getpid());
  stop = 1;
}

/// This is main...
int main(int argc,char* argv[]) {
  int i, ret;
  in_port_t fixedport = 0;
  struct sockaddr_storage fixedhost;
  struct addrinfo hints, *res, *reslist;
  int tcp;
  struct tracker_s * conn;

  memset(&fixedhost, '\0', sizeof(fixedhost));
  printf("netsed " VERSION " by Julien VdG <julien@silicone.homelinux.org>\n"
         "      based on 0.01c from Michal Zalewski <lcamtuf@ids.pl>\n");
  setbuffer(stdout,NULL,0);
  if (argc<6) usage_hints("not enough parameters");
  if (strcasecmp(argv[1],"tcp")*strcasecmp(argv[1],"udp")) usage_hints("incorrect protocol");
  tcp = strncasecmp(argv[1], "udp", 3);
  // allocate rule arrays, rule number is number of params after 5
  rule=malloc((argc-5)*sizeof(struct rule_s));
  rule_live=malloc((argc-5)*sizeof(int));
  // parse rules
  for (i=5;i<argc;i++) {
    char *fs=0, *ts=0, *cs=0;
    printf("[*] Parsing rule %s...\n",argv[i]);
    fs=strchr(argv[i],'/');
    if (!fs) error("missing first '/' in rule");
    fs++;
    ts=strchr(fs,'/');
    if (!ts) error("missing second '/' in rule");
    *ts=0;
    ts++;
    cs=strchr(ts,'/');
    if (cs) { *cs=0; cs++; }
    rule[rules].forig=fs;
    rule[rules].torig=ts;
    if (cs && *cs) /* Only non-trivial quantifiers count. */
      rule_live[rules]=atoi(cs); else rule_live[rules]=-1;
    shrink_to_binary(&rule[rules]);
//    printf("DEBUG: (%s) (%s)\n",rule[rules].from,rule[rules].to);
    rules++;
  }

  printf("[+] Loaded %d rule%s...\n", rules, (rules > 1) ? "s" : "");

  memset(&hints, '\0', sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_flags = AI_CANONNAME;
  hints.ai_socktype = tcp ? SOCK_STREAM : SOCK_DGRAM;

  if ((ret = getaddrinfo(argv[3], argv[4], &hints, &reslist))) {
    ERR("getaddrinfo(): %s\n", gai_strerror(ret));
    error("Impossible to resolve remote address or port.");
  }
  /* We have candidates for remote host. */
  for (res = reslist; res; res = res->ai_next) {
    int sd = -1;

    if ( (sd = socket(res->ai_family, res->ai_socktype, res->ai_protocol)) < 0)
      continue;
    /* Has successfully built a socket for this address family. */
    /* Record the address structure and the port. */
    fixedport = get_port(res->ai_addr);
    if (!is_addr_any(res->ai_addr))
      memcpy(&fixedhost, res->ai_addr, res->ai_addrlen);
    close(sd);
    break;
  }
  freeaddrinfo(reslist);
  if (res == NULL)
    error("Failed in resolving remote host.");

  if (fixedhost.ss_family && fixedport)
    printf("[+] Using fixed forwarding to %s,%s.\n",argv[3],argv[4]);
  else if (fixedport)
    printf("[+] Using dynamic (transparent proxy) forwarding with fixed port %s.\n",argv[4]);
  else if (fixedhost.ss_family)
    printf("[+] Using dynamic (transparent proxy) forwarding with fixed addr %s.\n",argv[3]);
  else
    printf("[+] Using dynamic (transparent proxy) forwarding.\n");

  bind_and_listen(fixedhost.ss_family, tcp, argv[2]);

  printf("[+] Listening on port %s/%s.\n", argv[2], argv[1]);

  signal(SIGPIPE, SIG_IGN);
  struct sigaction sa;
  sa.sa_flags = 0;
  sigemptyset(&sa.sa_mask);
  sa.sa_handler = sig_int;
  if (sigaction(SIGINT, &sa, NULL) == -1) error("netsed: sigaction() failed");

  while (!stop) {
    struct sockaddr_storage s;
    socklen_t l = sizeof(s);
    struct sockaddr_storage conho;
    in_port_t conpo;
    char ipstr[INET6_ADDRSTRLEN], portstr[12];

    int sel;
    fd_set rd_set;
    struct timeval timeout, *ptimeout;
    int nfds = lsock;
    FD_ZERO(&rd_set);
    FD_SET(lsock,&rd_set);
    timeout.tv_sec = UDP_TIMEOUT+1;
    timeout.tv_usec = 0;
    ptimeout = NULL;

    {
      conn = connections;
      while(conn != NULL) {
        if(tcp) {
          FD_SET(conn->csock, &rd_set);
          if (nfds < conn->csock) nfds = conn->csock;
        } else {
          // adjust timeout to earliest connection end time
          int remain = UDP_TIMEOUT - (now - conn->time);
          if (remain < 0) remain = 0;
          if (timeout.tv_sec > remain) {
            timeout.tv_sec = remain;
            // time updated to need to timeout
            ptimeout = &timeout;
          }
        }
        FD_SET(conn->fsock, &rd_set);
        if (nfds < conn->fsock) nfds = conn->fsock;
        // point on next
        conn = conn->n;
      }
    }

    sel=select(nfds+1, &rd_set, (fd_set*)0, (fd_set*)0, ptimeout);
    time(&now);
    if (stop)
    {
      break;
    }
    if (sel < 0) {
      DBG("[!] select fail! %s\n", strerror(errno));
      break;
    }
    if (sel == 0) {
      DBG("[*] select timeout. now: %d\n", now);
      // Here we still have to go through the list to expire some udp
      // connection if they timed out... But no descriptor will be set.
      // For tcp, select will not timeout.
    }

    if (FD_ISSET(lsock, &rd_set)) {
      int csock=-1;
      ssize_t rd=-1;
      if (tcp) {
        csock = accept(lsock,(struct sockaddr*)&s,&l);
      } else {
        // udp does not handle accept, so track connections manually
        // also set csock if a new connection need to be registered
        // to share the code with tcp ;)
        rd = recvfrom(lsock,buf,sizeof(buf),0,(struct sockaddr*)&s,&l);
        if(rd >= 0) {
          conn = connections;
          while(conn != NULL) {
            // look for existing connections
            if ((conn->csl == l) && (0 == memcmp(&s, conn->csa, l))) {
              // found
              break;
            }
            // point on next
            conn = conn->n;
          }
          // not found
          if(conn == NULL) {
            // udp 'connection' socket is the listening one
            csock = lsock;
          } else {
            DBG("[+] Got incoming datagram from existing connection.\n");
          }
        } else {
          ERR("recvfrom(): %s", strerror(errno));
        }
      }

      // new connection (tcp accept, or udp conn not found)
      if ((csock)>=0) {
        int one=1;
        getnameinfo((struct sockaddr *) &s, l, ipstr, sizeof(ipstr),
                    portstr, sizeof(portstr), NI_NUMERICHOST | NI_NUMERICSERV);
        printf("[+] Got incoming connection from %s,%s", ipstr, portstr);
        conn = malloc(sizeof(struct tracker_s));
        if(NULL == conn) error("netsed: unable to malloc() connection tracker struct");
        // protocol specific init
        if (tcp) {
          setsockopt(csock,SOL_SOCKET,SO_OOBINLINE,&one,sizeof(int));
          conn->csa = NULL;
          conn->csl = 0;
          conn->state = ESTABLISHED;
        } else {
          conn->csa = malloc(l);
          if(NULL == conn->csa) error("netsed: unable to malloc() connection tracker sockaddr struct");
          memcpy(conn->csa, &s, l);
          conn->csl = l;
          conn->state = UNREPLIED;
        }
        conn->csock = csock;
        conn->time = now;

        conn->live = malloc(rules*sizeof(int));
        if(NULL == conn->live) error("netsed: unable to malloc() connection tracker sockaddr struct");
        memcpy(conn->live, rule_live, rules*sizeof(int));

        l = sizeof(s);
#ifndef LINUX_NETFILTER
        // was OK for linux 2.2 nat
        getsockname(csock,(struct sockaddr*)&s,&l);
#else
        // for linux 2.4 and later
        getsockopt(csock, SOL_IP, SO_ORIGINAL_DST,(struct sockaddr*)&s,&l);
#endif
        getnameinfo((struct sockaddr *) &s, l, ipstr, sizeof(ipstr),
                    portstr, sizeof(portstr), NI_NUMERICHOST | NI_NUMERICSERV);
        printf(" to %s,%s\n", ipstr, portstr);
        conpo = get_port((struct sockaddr *) &s);

        memcpy(&conho, &s, sizeof(conho));

        if (fixedport) conpo=fixedport;
        if (fixedhost.ss_family)
          memcpy(&conho, &fixedhost, sizeof(conho));

        // forward to addr
        memcpy(&s, &conho, sizeof(s));
        set_port((struct sockaddr *) &s, conpo);
        getnameinfo((struct sockaddr *) &s, l, ipstr, sizeof(ipstr),
                    portstr, sizeof(portstr), NI_NUMERICHOST | NI_NUMERICSERV);
        printf("[*] Forwarding connection to %s,%s\n", ipstr, portstr);

        // connect will bind with some dynamic addr/port
        conn->fsock = socket(s.ss_family, tcp ? SOCK_STREAM : SOCK_DGRAM, 0);

	//bind_forward(conn->fsock, fixedhost.ss_family, tcp, "33333");

        if (connect(conn->fsock,(struct sockaddr*)&s,l)) {
           printf("[!] Cannot connect to remote server, dropping connection.\n");
           freetracker(conn);
           conn = NULL;
        } else {
          setsockopt(conn->fsock,SOL_SOCKET,SO_OOBINLINE,&one,sizeof(int));
          conn->n = connections;
          connections = conn;
        }
      }
      // udp has data process forwarding
      if((rd >= 0) && (conn != NULL)) {
        b2server_sed(conn, rd);
      }
    } // lsock is set
    // all other sockets
    conn = connections;
    struct tracker_s ** pconn = &connections;
    while(conn != NULL) {
      // incoming data ?
      if(tcp && FD_ISSET(conn->csock, &rd_set)) {
        client2server_sed(conn);
      }
      if(FD_ISSET(conn->fsock, &rd_set)) {
        server2client_sed(conn);
      }
      // timeout ? udp only
      DBG("[!] connection last time: %d, now: %d\n", conn->time, now);
      if(!tcp && ((now - conn->time) >= UDP_TIMEOUT)) {
        DBG("[!] connection timeout.\n");
        conn->state = TIMEOUT;
      }
      if(conn->state >= DISCONNECTED) {
        // remove it
        (*pconn)=conn->n;
        freetracker(conn);
        conn=(*pconn);
      } else {
        // point on next
        pconn = &(conn->n);
        conn = conn->n;
      }
    }
  }

  clean_socks();
  exit(0);
}

// vim:sw=2:sta:et:
