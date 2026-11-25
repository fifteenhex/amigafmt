/* amigafmt: the network databases behind bsdsocket.library (netdb.c) */
#ifndef NETDB_INT_H
#define NETDB_INT_H
#include <stdint.h>

#define ND_MAXALIAS 8
#define ND_MAXADDR 16

/* h_errno values (the same in BSD and Linux) */
#define ND_HOST_NOT_FOUND 1
#define ND_TRY_AGAIN 2
#define ND_NO_RECOVERY 3
#define ND_NO_DATA 4

struct nd_host {
	char name[256];
	char aliases[ND_MAXALIAS][256];
	int nalias;
	uint32_t addr[ND_MAXADDR];      /* network order */
	int naddr;
};

struct nd_ent {
	char name[64];
	char aliases[ND_MAXALIAS][64];
	int nalias;
	uint32_t num;                   /* port, protocol or network number */
	char proto[16];                 /* services only */
};

int nd_gethostbyname(const char *name, struct nd_host *h);
int nd_gethostbyaddr(uint32_t addr, struct nd_host *h);
int nd_getserv(const char *name, int port, const char *proto, struct nd_ent *e);
int nd_getproto(const char *name, int num, struct nd_ent *e);
int nd_getnet(const char *name, uint32_t net, struct nd_ent *e);
uint32_t nd_inet_network(const char *cp, int *ok);
extern volatile int nd_interrupted;

#endif
