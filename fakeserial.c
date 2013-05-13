/* Tony Cheneau <tony.cheneau@nist.gov> */

/*
* Conditions Of Use
*
* This software was developed by employees of the National Institute of
* Standards and Technology (NIST), and others.
* This software has been contributed to the public domain.
* Pursuant to title 15 Untied States Code Section 105, works of NIST
* employees are not subject to copyright protection in the United States
* and are considered to be in the public domain.
* As a result, a formal license is not needed to use this software.
*
* This software is provided "AS IS."
* NIST MAKES NO WARRANTY OF ANY KIND, EXPRESS, IMPLIED
* OR STATUTORY, INCLUDING, WITHOUT LIMITATION, THE IMPLIED WARRANTY OF
* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE, NON-INFRINGEMENT
* AND DATA ACCURACY.  NIST does not warrant or make any representations
* regarding the use of the software or the results thereof, including but
* not limited to the correctness, accuracy, reliability or usefulness of
* this software.
*/

#define _XOPEN_SOURCE 500
#include <stdlib.h>

#include <sys/select.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <getopt.h>
#include <errno.h>
#include <termios.h>

#undef max
#define max(x,y) ((x) > (y) ? (x) : (y))

#ifdef DEBUG
#define PRINTF(...) printf(__VA_ARGS__)
#else
#define PRINTF(...)
#endif

/* Linux serial command constants */
#define START_BYTE1 'z'
#define START_BYTE2 'b'

#define   OPEN          0x01
#define   CLOSE         0x02
#define   SET_CHANNEL   0x04
#define   ED            0x05
#define   CCA           0x06
#define   SET_STATE     0x07
#define   TX_BLOCK      0x09
#define   RX_BLOCK      0x0b
#define   GET_ADDR      0x0d
#define   SET_PANID     0x0f
#define   SET_SHORTADDR 0x10
#define   SET_LONGADDR  0x11

#define RESP_MASK 0x80

#define SUCCESS 0x00

#define IEEE802154_LONG_ADDR_LEN 8
#define IEEE802154_SHORT_ADDR_LEN 2

#define SINGLE_CONNECTION 1
/* 127 (max frame size) + 5 (max command size) */
#define BUFSIZE 132

#define BAUDRATE 921600

#define HAVE_GETOPT_LONG

#ifdef HAVE_GETOPT_LONG
static const struct option iz_long_opts[] = {
	{ "udp-remote-port", required_argument, NULL, 'r' },
	{ "baudrate", required_argument, NULL, 'b' },
	{ "device-name", required_argument, NULL, 'n'},
	{ "udp-dest", required_argument, NULL, 'd' },
	{ "udp-local-port", required_argument, NULL, 'l'},
	{ "version", no_argument, NULL, 'v' },
	{ "help", no_argument, NULL, 'h' },
	{ NULL, 0, NULL, 0 },
};
#endif

/* global variables */

static uint16_t panid;
static unsigned char ieee802154_long_addr[IEEE802154_LONG_ADDR_LEN];
static unsigned char ieee802154_short_addr[IEEE802154_SHORT_ADDR_LEN];
static int serialfd = 0;
static char * devname = "fakeserial0";
static int baudrate = BAUDRATE;

void print_version() {
	printf("This software is provided \"AS IS.\"\n"
		    "NIST MAKES NO WARRANTY OF ANY KIND, EXPRESS, IMPLIED"
		    "OR STATUTORY, INCLUDING, WITHOUT LIMITATION, THE IMPLIED WARRANTY OF"
		    "MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE, NON-INFRINGEMENT"
		    "AND DATA ACCURACY.  NIST does not warrant or make any representations"
		    "regarding the use of the software or the results thereof, including but"
		    "not limited to the correctness, accuracy, reliability or usefulness of"
		    "this software.\n"
			"\n"
			"Written by Tony Cheneau <tony.cheneau@nist.gov>\n");

	exit(EXIT_SUCCESS);
}

void print_usage(const char * prgname) {
	printf("This program mimics the behavior of a IEEE 802.15.4 Serial device (e.g. RedBee Econotag)\n\n");

	printf("usage: %s -d destaddr -l portnum -r portnum [-b baudrate] [-n devicename]\n", prgname);
	printf("-b, --baudrate: baudrate of the fake serial port (default \"%d\")\n"
		   "-n, --device-name: name of the fake serial port (default \"/dev/fakeserial0\")\n"
		   "-d, --udp-dest: destination address for the UDP traffic sent to the backend\n"
		   "-l, --udp-local-port: local udp port to be bound\n"
		   "-r, --udp-remote-port: remote UDP port to connect to and to bind locally\n"
		   "-h, --help: this help message\n"
		   "-v, --version: print program version and exits\n", BAUDRATE);
}

/* return the file descriptor to the fake serial device */
int set_serial(char * devname, int baudrate){
	int fd;
	struct termios tbuf;
	char * ptmaster;

	fd = open("/dev/ptmx", O_RDWR);
	if (fd < 0) {
		perror("open");
		exit(EXIT_FAILURE);
	}

	if ( grantpt(fd) < 0 ) {
		perror("grantpt");
		exit(EXIT_FAILURE);
	}

	if ( unlockpt(fd) < 0 ) {
		perror("unlockpt");
		exit(EXIT_FAILURE);
	}

	/* create the serial port entry in /dev */
	unlink(devname);
	ptmaster = ptsname(fd);
	if (!ptmaster) {
		perror("ptsname");
		exit(EXIT_FAILURE);
	}

	if ( symlink(ptmaster, devname) ) {
		perror("symlink");
		exit(EXIT_FAILURE);
	}

	/* set, among other things, the baudrate of the pseudo-terminal */

	memset(&tbuf, 0, sizeof(tbuf));

	tbuf.c_iflag |= IGNBRK;
	tbuf.c_cflag |= CLOCAL | CREAD | CS8;
	tbuf.c_cc[VMIN] = 1;
	tbuf.c_cc[VTIME] = 5;

	switch (baudrate){
	case 115200:
		cfsetospeed(&tbuf, B115200);
		cfsetispeed(&tbuf, B115200);
		break;
	case 921600:
		cfsetospeed(&tbuf, B921600);
		cfsetispeed(&tbuf, B921600);
		break;
	default:
		fprintf(stderr, "speed %d is not supported\n", baudrate);
	    exit(EXIT_FAILURE);
	}

	if ( tcsetattr(fd, TCSANOW, &tbuf) < 0 ) {
		perror("tcsetattr");
		exit(EXIT_FAILURE);
	}

	return fd;
}


int client_setup(struct sockaddr * dest_addr, socklen_t * addr_len,
				 const char * dst, const char * lport,
				 const char * dport) {
    struct addrinfo hints;
    struct addrinfo *result, *rp;
	int s, sfd;

	   /* Obtain address(es) matching host/dport */

	   memset(&hints, 0, sizeof(struct addrinfo));
	   hints.ai_family = AF_UNSPEC;	   /* Allow IPv4 or IPv6 */
	   hints.ai_socktype = SOCK_DGRAM; /* Datagram socket */
	   hints.ai_flags = 0;
	   hints.ai_protocol = 0;	   /* Any protocol */

	   s = getaddrinfo(dst, dport, &hints, &result);
	   if (s != 0) {
	       fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(s));
	       exit(EXIT_FAILURE);
	   }

	   /* getaddrinfo() returns a list of address structures.
	      Try each address until we successfully connect(2).
	      If socket(2) (or connect(2)) fails, we (close the socket
	      and) try the next address. */

	   for (rp = result; rp != NULL; rp = rp->ai_next) {
	       sfd = socket(rp->ai_family, rp->ai_socktype,
			    rp->ai_protocol);
	       if (sfd == -1)
			   continue;

		   if (connect(sfd, rp->ai_addr, rp->ai_addrlen) != -1) {
			   int ret = -1, yes = 1, sendbuff = 2048;
			   struct sockaddr unspec;
			   memcpy(dest_addr, rp->ai_addr, sizeof(struct sockaddr));
			   * addr_len = rp->ai_addrlen;

			   memset(&unspec, 0, sizeof(struct sockaddr));
			   unspec.sa_family = AF_UNSPEC;
			   /* un-connect the socket */
			   if (connect(sfd, &unspec, sizeof(struct sockaddr)) < 0) {
				   PRINTF("unable to connect(AF_UNSPEC): %s\n", strerror(errno));
				   close(sfd);
				   continue;
			   }

			   if (setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &yes,
							  sizeof(int)) == -1) {
				   perror("setsockopt()");
				   exit(EXIT_FAILURE);
			   }

			   if (setsockopt(sfd, SOL_SOCKET, SO_SNDBUF, &sendbuff,
						   sizeof(sendbuff)) == -1) {
				   perror("setsockopt()");
				   exit(EXIT_FAILURE);
			   }


			   if (rp->ai_family == AF_INET)
			   {
				   struct sockaddr_in serv_addr;

				   memset(&serv_addr, 0, sizeof(struct sockaddr_in));
				   serv_addr.sin_family = AF_INET;
				   serv_addr.sin_addr.s_addr = INADDR_ANY;
				   serv_addr.sin_port = htons(atoi(lport));

				   ret = bind(sfd, (struct sockaddr *) &serv_addr, sizeof(serv_addr));
			   } else if (rp->ai_family == AF_INET6) {
				   struct sockaddr_in6 serv_addr;

				   memset(&serv_addr, 0, sizeof(struct sockaddr_in6));
				   serv_addr.sin6_family = AF_INET6;
				   serv_addr.sin6_addr = in6addr_any;
				   serv_addr.sin6_port = htons(atoi(lport));

				   ret = bind(sfd, (struct sockaddr *) &serv_addr, sizeof(serv_addr));
			   }
			   else {
					fprintf(stderr, "address family not supported\n");
					exit(EXIT_FAILURE);
			   }

			   if ( ret < 0 )
			   {
				   PRINTF("unable to bind() the UDP socket: %s\n", strerror(errno));
				   close(sfd);
				   continue;
			   }

			   /* bind the socket to a local port */
			   break; /* Success */
		   }

	       close(sfd);
	   }

	   if (rp == NULL) { /* No address succeeded */
	       fprintf(stderr, "Could not connect\n");
	       exit(EXIT_FAILURE);
	   }

	   freeaddrinfo(result);	   /* No longer needed */

	   return sfd;
}


/* read a single character and returns it
 * when this function is called, a character must be ready on the file
 * descriptor */
unsigned char read_one_byte() {
	unsigned char buf[1] = { 0 };
	ssize_t bytes = 0;
	fd_set readfds;

	FD_ZERO(&readfds);
	FD_SET(serialfd, &readfds);

	if ( 0 >= select(serialfd+1, &readfds, NULL, NULL, NULL) ) {
		perror("select()");
		exit(EXIT_FAILURE);
	}

	bytes = read(serialfd, buf, 1);

	if ( bytes <= 0 ) {
		if (errno == EIO) {
			PRINTF("closed connection to the serial port\n");
			close(serialfd);
			while ( (serialfd = set_serial(devname, baudrate)) < 0 ){
				PRINTF("unable to reopen serial port\n");
			}
		} else {
			perror("read");
			exit(EXIT_FAILURE);
		}
	}

	/* PRINTF("%02X", buf[0]); */

	return buf[0];
}

/* send a success message that matches the command */
void send_success(unsigned char type) {
	unsigned char buf[4] = { START_BYTE1,
							 START_BYTE2,
							 0, /* cmd */
							 SUCCESS};

	buf[2] = type | RESP_MASK; /* compute the response type */
	if ( write(serialfd, buf, 4) < 0 ) {
		perror("write");
		exit(EXIT_FAILURE);
	}

	return;
}

/* parse command from the linux serial driver
 * see http://sourceforge.net/apps/trac/linux-zigbee/wiki/SerialV1 */
void parse_cmd(int tosock, struct sockaddr * dest_addr, socklen_t dest_addr_len) {
	char buf[BUFSIZE] = { START_BYTE1, START_BYTE2 };
	unsigned char cmd_type;

	if ( START_BYTE1 != read_one_byte() )
		return;

	PRINTF("received 'z'\n");

	if ( START_BYTE2 != read_one_byte() )
		return;

	PRINTF("received 'b'\n");

	cmd_type = read_one_byte();

	PRINTF("parse_cmd: received a command of type %d\n", cmd_type);

	switch (cmd_type) {
		case SET_PANID: {
							unsigned char hi, lo;
							hi = read_one_byte();
							lo = read_one_byte();
							panid = hi << 8 | lo;
							send_success(cmd_type);
							break;
						}
		case SET_SHORTADDR:
						ieee802154_short_addr[1] = read_one_byte();
						ieee802154_short_addr[0] = read_one_byte();
						send_success(cmd_type);
						break;
		case SET_LONGADDR:
						if ( read(serialfd, ieee802154_long_addr,
									IEEE802154_LONG_ADDR_LEN) < 0) {
							perror("read");
							exit(EXIT_FAILURE);
						}

						send_success(cmd_type);
						break;
		case GET_ADDR: {
						   int i = 0;
						   buf[2] = cmd_type | RESP_MASK;
						   buf[3] = SUCCESS;
						   /* fill out the rest of the buffer */
						   for(i=0; i< IEEE802154_LONG_ADDR_LEN; i++)
							   buf[4+i] = ieee802154_long_addr[i];
						   write(serialfd, buf, 2 + 1 + 1 + IEEE802154_LONG_ADDR_LEN);
						   break;
					   }
		case TX_BLOCK: {
						   unsigned char len = 0;
						   len =  read_one_byte();
						   if ( read(serialfd, buf, len) != len ) {
							   perror("read");
							   exit(EXIT_FAILURE);
						   }

						   PRINTF("parse_cmd: sending IEEE 802.15.4 frame to the backend\n");
						   if (sendto(tosock, buf, len, 0, dest_addr, dest_addr_len) < 0) {
							   perror("sendto()");
							   exit(EXIT_FAILURE);
						   }
						   send_success(cmd_type);
						   break;
					   }
		case SET_CHANNEL:
					   /* currently ignore the channel being set */
					   read_one_byte();
					   send_success(cmd_type);
					   break;
		default:
					   /* OPEN, CLOSE, ED, CCA, SET_STATE */
					   send_success(cmd_type);
	}

	return;
}

void send_to_linux(int fromsock) {
	unsigned char buf[BUFSIZE];
	ssize_t msg_size;
	struct msghdr msg;
	struct iovec iov;

	/* Receive block command */
	buf[0] = 'z';
	buf[1] = 'b';
	buf[2] = 0x8b;
	/* LQI */
	buf[3] = 0;

	iov.iov_base = &buf[3 + 1 + 1];
	iov.iov_len = BUFSIZE - (3 + 1 + 1);

	msg.msg_name = NULL;
	msg.msg_namelen = 0;
	msg.msg_iov = & iov;
	msg.msg_iovlen = 1;
	msg.msg_flags = 0;
	msg.msg_control = NULL;
	msg.msg_controllen = 0;

	/* message length */
	msg_size = recvmsg(fromsock, &msg, 0);
	buf[4] = msg_size;

	if (msg_size <= 0) {
		perror("recvmsg()");
		exit(EXIT_FAILURE);
	}

	/* inject the packet in the Linux network stack */
	write(serialfd, buf, 3 + 1 + 1 + msg_size);

	return;
}

int main(int argc, char *argv[]) {
	int udpsock;
	int c, nfds;
	fd_set readfds;
	char * clidest = NULL;
	char * udp_dport = NULL;
	char * udp_lport = NULL;
	struct sockaddr dest_addr;
	socklen_t dest_addr_len = 0;

	/* parse the arguments with getopt */
	while (1) {
#ifdef HAVE_GETOPT_LONG
		int opt_idx = -1;
		c = getopt_long(argc, argv, "b:n:d:l:r:vh", iz_long_opts, &opt_idx);
#else
		c = getopt(argc, argv, "b:n:d:l:r:vh");
#endif
		if (c == -1)
			break;

		switch(c) {
			case 'd':
				clidest = optarg;
				break;
			case 'b':
				baudrate = atoi(optarg);
				break;
			case 'n':
				devname = optarg;
			case 'r':
				udp_dport = optarg;
				break;
			case 'l':
				udp_lport = optarg;
				break;
			case 'v':
				print_version();
				return 0;
			case 'h':
			default:
				print_usage(argv[0]);
				return 1;
		}
	}

	if (optind < argc) {
		printf("some arguments could not be parsed\n");
		print_usage(argv[0]);
		return -1;
	}

	if ( !(udp_lport && clidest && udp_dport) ){
		printf("-l, -r, and -d arguments must be set\n");
		exit(EXIT_FAILURE);
	}

	/* set the fake serial port */
	serialfd = set_serial(devname, baudrate);

	/* open the client socket where the IEEE 802.15.4 MAC frames will be redirected */
	udpsock = client_setup(&dest_addr, &dest_addr_len, clidest, udp_lport, udp_dport);

	if ( udpsock < 0 ) {
		perror("client_setup()");
		exit(EXIT_FAILURE);
	}

	/* start the processing loop */
	while (1) {
		FD_ZERO(&readfds);
		FD_SET(serialfd, &readfds);
		FD_SET(udpsock, &readfds);
		nfds = max(serialfd, udpsock);

		PRINTF("select: waiting for new activity\n");
		if ( 0 >= select(nfds + 1, &readfds, NULL, NULL, NULL)) {
			perror("select()");
			exit(EXIT_FAILURE);
		}

		if (FD_ISSET(udpsock, &readfds)) {
			PRINTF("select: received a packet from backend\n");
			/* pass the packet to the kernel */
			send_to_linux(udpsock);
		}
		if (FD_ISSET(serialfd, &readfds)) {
			PRINTF("select: received a packet from the fake serial device\n");
			/* need to parse the serial protocol */
			parse_cmd(udpsock, &dest_addr, dest_addr_len);
		}
	}

	unlink(devname);
	close(udpsock);
	close(serialfd);
	return 0;
}
