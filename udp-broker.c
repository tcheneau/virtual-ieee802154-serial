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

#include<stdio.h>
#include<unistd.h>
#include<getopt.h>
#include<string.h>
#include<sys/select.h>
#include<sys/types.h>
#include<sys/time.h>
#include<fcntl.h>
#include<sys/socket.h>
#include<stdlib.h>
#include<netdb.h>
#include<string.h>

#ifdef DEBUG
#define PRINTF(...) printf(__VA_ARGS__)
#else
#define PRINTF(...)
#endif

#define BUFSIZE 2048

#define HAVE_GETOPT_LONG

#ifdef HAVE_GETOPT_LONG
static const struct option iz_long_opts[] = {
	{ "local-port", required_argument, NULL, 'l' },
    { "write", required_argument, NULL, 'w' },
	{ "version", no_argument, NULL, 'v' },
	{ "help", no_argument, NULL, 'h' },
	{ NULL, 0, NULL, 0 },
};
#endif

struct client_list {
	struct sockaddr addr;
	socklen_t addrlen;
	struct client_list * next;
};


/* create a new list and initialize it with its first element */
struct client_list * list_init (struct sockaddr * addr, socklen_t addrlen) {
	struct client_list * head;

	head = (struct client_list *) malloc(sizeof(struct client_list));

	if (!head) {
		perror("malloc()");
		exit(EXIT_FAILURE);
	}

	memcpy(&head->addr, addr, sizeof(struct sockaddr));
	head->addrlen = addrlen;
	head->next = NULL;

	return head;
}

/* find an element in the list or return NULL if none is found */
struct client_list * list_find(struct client_list * list, struct sockaddr * addr, socklen_t addrlen) {
	struct client_list * p;
	for (p = list; p; p = p->next)
		if (memcmp(p, addr, sizeof(struct sockaddr)) == 0 &&
			p->addrlen == addrlen)
			return p;

	return NULL;
}

/* add a new element at the beginning of the list */
struct client_list * list_add(struct client_list * list, struct sockaddr * addr, socklen_t addrlen) {
	struct client_list * head;

	head = (struct client_list *) malloc(sizeof(struct client_list));

	if (!head) {
		perror("malloc()");
		exit(EXIT_FAILURE);
	}

	memcpy(&head->addr, addr, sizeof(struct sockaddr));
	head->addrlen = addrlen;
	head->next = list;

	return head;
}

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
    printf("Version 0.1\n");

	exit(EXIT_SUCCESS);
}

void print_usage(const char * prgname) {
	printf("This program a minimalist UDP broker.\n"
			"A new client is first detected when it sends a message to the broker.\n"
			"Subsequently, all messages received by the broker will be send to"
			"all the clients (except the one sending the message)\n");

	printf("usage: %s -l portnum [-w pcapfile]\n", prgname);
	printf("-l, --local-port: local udp port to be bound\n");
	printf("-w, --write: write all the packet to a pcap file\n");
	printf("-v, --version: print the program version\n");
	printf("-h, --help: print this help message\n");
}

int ipv6_server_setup(const char * lport) {
    struct addrinfo hints;
    struct addrinfo *result, *rp;
    int sfd, s, yes = 1;

    memset(&hints, 0, sizeof(struct addrinfo));
    hints.ai_family = AF_UNSPEC;    /* Allow IPv4 or IPv6 */
    hints.ai_socktype = SOCK_DGRAM; /* Datagram socket */
    hints.ai_flags = AI_PASSIVE;    /* For wildcard IP address */
    hints.ai_protocol = 0;          /* Any protocol */
    hints.ai_canonname = NULL;
    hints.ai_addr = NULL;
    hints.ai_next = NULL;

    s = getaddrinfo(NULL, lport, &hints, &result);
    if (s != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(s));
        exit(EXIT_FAILURE);
    }

    /* getaddrinfo() returns a list of address structures.
       Try each address until we successfully bind(2).
       If socket(2) (or bind(2)) fails, we (close the socket
       and) try the next address. */
    for (rp = result; rp != NULL; rp = rp->ai_next) {
        sfd = socket(rp->ai_family, rp->ai_socktype,
                rp->ai_protocol);
        if (sfd == -1)
            continue;

        if (bind(sfd, rp->ai_addr, rp->ai_addrlen) == 0)
            break;                  /* Success */

        close(sfd);
    }

    if (rp == NULL) {               /* No address succeeded */
        fprintf(stderr, "Could not bind\n");
        exit(EXIT_FAILURE);
    }

    freeaddrinfo(result);           /* No longer needed */

	if (setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &yes,
				   sizeof(int)) == -1) {
		perror("setsockopt()");
		exit(EXIT_FAILURE);
	}

	return sfd;
}

/* from the pcap.h */
struct pcap_file_header
{
    uint32_t    magic;
    uint16_t    version_major;
    uint16_t    version_minor;
    int32_t     thiszone;
    uint32_t    sigfigs;
    uint32_t    snaplen;
    uint32_t    linktype;
};

struct pcap_pkthdr {
        uint32_t ts_sec;     /* time stamp (second) */
        uint32_t ts_msec;    /* time stamp (microseconds) */
        uint32_t caplen;     /* length of portion present */
        uint32_t len;        /* length this packet (off wire) */
};

/* PCAP related helper function */

void pcap_write_header(int fd) {
    struct pcap_file_header header;

    /* see pcap-savefile(5) */
    header.magic = 0xa1b2c3d4;
    header.version_major = 2;
    header.version_minor = 4;
    header.thiszone = 0;
    header.sigfigs = 0;
    header.snaplen = 127; /* max MTU on IEEE 802.15.4 links */
    header.linktype = 230; /* IEEE 802.15.4 */

    if (write(fd, &header, sizeof(header)) < 0) {
        perror("write");
        exit(EXIT_FAILURE);
    }
}

void pcap_write_packet(int fd, char * packet, size_t packet_len) {
    struct pcap_pkthdr header;
    struct timeval tv;

    memset(&header, 0, sizeof(header));

    gettimeofday(&tv, NULL);
    /* force casting to 32 bit (struct timeval could be storing in 64 bit format)*/
    header.ts_sec = (uint32_t) tv.tv_sec;
    header.ts_msec = (uint32_t) tv.tv_usec;

    header.caplen = packet_len;
    header.len = packet_len;

    if (write(fd, &header, sizeof(header)) < 0)
        goto err;

    if (write(fd, packet, packet_len) < 0)
        goto err;


    return;

err:
    perror("write");
    exit(EXIT_FAILURE);
}

int main(int argc, char *argv[]) {
	int udpsock;
    int pcap_fd;
    unsigned long int packet_seq = 0;
	int c;
	fd_set readfds;
	char * udp_lport = NULL, * pcap_file = NULL;
	char buffer[BUFSIZE];
	struct sockaddr client_addr;
	socklen_t client_addr_len, len = 0;
	struct client_list *p, * client, * client_list = NULL;

	/* parse the arguments with getopt */
	while (1) {
#ifdef HAVE_GETOPT_LONG
		int opt_idx = -1;
		c = getopt_long(argc, argv, "w:l:vh", iz_long_opts, &opt_idx);
#else
		c = getopt(argc, argv, "w:l:vh");
#endif
		if (c == -1)
			break;

		switch(c) {
		case 'l':
			udp_lport = optarg;
			break;
		case 'v':
			print_version();
			return 0;
        case 'w':
            pcap_file = optarg;
            break;
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

	if ( !udp_lport ){
		print_usage(argv[0]);
		printf("\n\nerror: --local-port argument must be set\n");
		exit(EXIT_FAILURE);
	}

    if (pcap_file) {
        PRINTF("opening pcap file %s\n", pcap_file);
        pcap_fd = open(pcap_file, O_CREAT|O_WRONLY|O_TRUNC, S_IRUSR|S_IRGRP);

        if (pcap_fd < 0) {
            perror("open");
            exit(EXIT_FAILURE);
        }

        pcap_write_header(pcap_fd);
    }



	/* open the broker socket */
	udpsock = ipv6_server_setup(udp_lport);

	if ( udpsock < 0 ) {
		perror("ipv6_server_setup()");
		exit(EXIT_FAILURE);
	}

	/* start the processing loop */
	while (1) {
		FD_ZERO(&readfds);
		FD_SET(udpsock, &readfds);

		PRINTF("select: waiting for activity\n");
		if ( 0 >= select(udpsock + 1, &readfds, NULL, NULL, NULL)) {
			perror("select()");
			exit(EXIT_FAILURE);
		}

		if (FD_ISSET(udpsock, &readfds)) {
			PRINTF("select: received a packet (%lu)\n", packet_seq++);
			len = recvfrom(udpsock, buffer, BUFSIZE, 0, &client_addr, &client_addr_len);
			if (len < 0) {
				perror("recvfrom()");
				exit(EXIT_FAILURE);
			}

            if (pcap_file)
                pcap_write_packet(pcap_fd, buffer, len);


			if ( (client = list_find(client_list, &client_addr, client_addr_len)) == NULL )
			{
				PRINTF("received a message from a new client, registering the client\n");
				if (client_list)
					client_list = list_add(client_list, &client_addr, client_addr_len);
				else
					client_list = list_init(&client_addr, client_addr_len);

				client = list_find(client_list, &client_addr, client_addr_len);
			}

			for (p=client_list; p; p=p->next) {
				if (p == client) /* do not send to self */
					continue;
				sendto(udpsock, buffer, len, 0, &p->addr, p->addrlen);
			}
		}
	}

	close(udpsock);
    close(pcap_fd);
	return 0;
}
