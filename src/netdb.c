/*
 * The network databases behind bsdsocket.library's gethostby*, getserv*,
 * getproto* and getnet*: /etc/hosts and a small DNS client of our own
 * (UDP to the servers in /etc/resolv.conf), /etc/services, /etc/protocols
 * and /etc/networks, each with built-in entries for a system that has no
 * such file (a busybox root often has none).
 *
 * glibc's own resolver is not used on purpose: amigafmt is linked
 * statically, and a static glibc's NSS wants the shared libraries of the
 * exact glibc it was built with at run time, which a small m68k system does not have.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include "netdb_int.h"

/* ---- the text databases ---- */

static const char builtin_services[] =
	"echo 7/tcp\necho 7/udp\ndiscard 9/tcp sink null\ndiscard 9/udp sink null\n"
	"systat 11/tcp users\ndaytime 13/tcp\ndaytime 13/udp\nnetstat 15/tcp\n"
	"qotd 17/tcp quote\nchargen 19/tcp ttytst source\nchargen 19/udp ttytst source\n"
	"ftp-data 20/tcp\nftp 21/tcp\nssh 22/tcp\ntelnet 23/tcp\nsmtp 25/tcp mail\n"
	"time 37/tcp timserver\ntime 37/udp timserver\nnameserver 42/tcp name\n"
	"whois 43/tcp nicname\ndomain 53/tcp\ndomain 53/udp\nbootps 67/udp\n"
	"bootpc 68/udp\ntftp 69/udp\ngopher 70/tcp\nfinger 79/tcp\nhttp 80/tcp www www-http\n"
	"pop3 110/tcp pop-3\nsunrpc 111/tcp portmapper\nsunrpc 111/udp portmapper\n"
	"auth 113/tcp authentication tap ident\nnntp 119/tcp readnews untp\n"
	"ntp 123/udp\nimap2 143/tcp imap\nsnmp 161/udp\nirc 194/tcp\nimap3 220/tcp\n"
	"https 443/tcp\nexec 512/tcp\nbiff 512/udp comsat\nlogin 513/tcp\n"
	"who 513/udp whod\nshell 514/tcp cmd\nsyslog 514/udp\nprinter 515/tcp spooler\n"
	"talk 517/udp\nntalk 518/udp\nuucp 540/tcp uucpd\nsocks 1080/tcp\n"
	"ircd 6667/tcp\n";

static const char builtin_protocols[] =
	"ip 0 IP\nicmp 1 ICMP\nigmp 2 IGMP\nggp 3 GGP\nipencap 4 IP-ENCAP\n"
	"tcp 6 TCP\negp 8 EGP\npup 12 PUP\nudp 17 UDP\nidp 22 IDP\nrdp 27 RDP\n"
	"ipv6 41 IPv6\nipv6-icmp 58 IPv6-ICMP\nraw 255 RAW\n";

static const char builtin_networks[] =
	"loopback 127\nlink-local 169.254\n";

/* the next line of a file or of a built-in text; 0 at the end */
struct src {
	FILE *f;
	const char *p;
};

static int src_line(struct src *s, char *buf, int len)
{
	if (s->f)
		return fgets(buf, len, s->f) != NULL;
	if (!*s->p)
		return 0;
	int n = 0;
	while (*s->p && *s->p != '\n' && n < len - 1)
		buf[n++] = *s->p++;
	if (*s->p == '\n')
		s->p++;
	buf[n] = 0;
	return 1;
}

static void src_open(struct src *s, const char *path, const char *builtin)
{
	s->f = fopen(path, "r");
	s->p = builtin;
}

static void src_close(struct src *s)
{
	if (s->f)
		fclose(s->f);
}

/* split a line into words, a '#' ends it; returns the count */
static int words(char *line, char **w, int max)
{
	int n = 0;
	char *p = strchr(line, '#');

	if (p)
		*p = 0;
	for (p = strtok(line, " \t\r\n"); p && n < max; p = strtok(NULL, " \t\r\n"))
		w[n++] = p;
	return n;
}

static void ent_fill(struct nd_ent *e, char **w, int n)
{
	memset(e, 0, sizeof(*e));
	snprintf(e->name, sizeof(e->name), "%s", w[0]);
	for (int i = 2; i < n && e->nalias < ND_MAXALIAS; i++)
		snprintf(e->aliases[e->nalias++], sizeof(e->aliases[0]), "%s", w[i]);
}

static int name_matches(char **w, int n, const char *name)
{
	if (!strcasecmp(w[0], name))
		return 1;
	for (int i = 2; i < n; i++)
		if (!strcasecmp(w[i], name))
			return 1;
	return 0;
}

/* services: "name port/proto aliases"; by name (port < 0) or by port */
int nd_getserv(const char *name, int port, const char *proto, struct nd_ent *e)
{
	struct src s;
	char line[512], *w[2 + ND_MAXALIAS];
	int n, found = 0;

	src_open(&s, "/etc/services", builtin_services);
	while (!found && src_line(&s, line, sizeof(line))) {
		char *sl;
		if ((n = words(line, w, 2 + ND_MAXALIAS)) < 2 ||
		    !(sl = strchr(w[1], '/')))
			continue;
		*sl++ = 0;
		if (proto && *proto && strcasecmp(sl, proto))
			continue;
		if (port >= 0 ? atoi(w[1]) == port : name_matches(w, n, name)) {
			ent_fill(e, w, n);
			e->num = atoi(w[1]);
			snprintf(e->proto, sizeof(e->proto), "%s", sl);
			found = 1;
		}
	}
	src_close(&s);
	return found;
}

/* protocols: "name number aliases" */
int nd_getproto(const char *name, int num, struct nd_ent *e)
{
	struct src s;
	char line[512], *w[2 + ND_MAXALIAS];
	int n, found = 0;

	src_open(&s, "/etc/protocols", builtin_protocols);
	while (!found && src_line(&s, line, sizeof(line))) {
		if ((n = words(line, w, 2 + ND_MAXALIAS)) < 2)
			continue;
		if (name ? name_matches(w, n, name) : atoi(w[1]) == num) {
			ent_fill(e, w, n);
			e->num = atoi(w[1]);
			found = 1;
		}
	}
	src_close(&s);
	return found;
}

/* a network number as inet_network() reads it: "a.b.c.d" right-aligned */
uint32_t nd_inet_network(const char *cp, int *ok)
{
	uint32_t v = 0, part;
	int parts = 0;
	char *end;

	*ok = 0;
	for (;;) {
		if (parts == 4)
			return 0;
		part = strtoul(cp, &end, 0);
		if (end == cp || part > 255)
			return 0;
		v = v << 8 | part;
		parts++;
		cp = end;
		if (*cp == '.') {
			cp++;
			continue;
		}
		break;
	}
	if (*cp && *cp != ' ' && *cp != '\t' && *cp != '\n')
		return 0;
	*ok = 1;
	return v;
}

/* networks: "name number aliases" */
int nd_getnet(const char *name, uint32_t net, struct nd_ent *e)
{
	struct src s;
	char line[512], *w[2 + ND_MAXALIAS];
	int n, found = 0, ok;

	src_open(&s, "/etc/networks", builtin_networks);
	while (!found && src_line(&s, line, sizeof(line))) {
		uint32_t v;
		if ((n = words(line, w, 2 + ND_MAXALIAS)) < 2)
			continue;
		v = nd_inet_network(w[1], &ok);
		if (!ok)
			continue;
		if (name ? name_matches(w, n, name) : v == net) {
			ent_fill(e, w, n);
			e->num = v;
			found = 1;
		}
	}
	src_close(&s);
	return found;
}

/* ---- hosts ---- */

static void host_add_alias(struct nd_host *h, const char *a)
{
	if (!strcasecmp(a, h->name) || h->nalias >= ND_MAXALIAS)
		return;
	for (int i = 0; i < h->nalias; i++)
		if (!strcasecmp(h->aliases[i], a))
			return;
	snprintf(h->aliases[h->nalias++], sizeof(h->aliases[0]), "%s", a);
}

static void host_add_addr(struct nd_host *h, uint32_t a)
{
	for (int i = 0; i < h->naddr; i++)
		if (h->addr[i] == a)
			return;
	if (h->naddr < ND_MAXADDR)
		h->addr[h->naddr++] = a;
}

/* /etc/hosts: by name (name != NULL) or by address; IPv4 lines only */
static int hosts_file(const char *name, uint32_t addr, struct nd_host *h)
{
	FILE *f = fopen("/etc/hosts", "r");
	char line[1024], *w[2 + ND_MAXALIAS];
	int n, found = 0;
	struct in_addr ia;

	if (!f)
		goto builtin;
	while (fgets(line, sizeof(line), f)) {
		if ((n = words(line, w, 2 + ND_MAXALIAS)) < 2 ||
		    inet_pton(AF_INET, w[0], &ia) != 1)
			continue;
		if (name) {
			int hit = 0;
			for (int i = 1; i < n; i++)
				if (!strcasecmp(w[i], name))
					hit = 1;
			if (!hit)
				continue;
		} else if (ia.s_addr != addr)
			continue;
		if (!found) {
			memset(h, 0, sizeof(*h));
			snprintf(h->name, sizeof(h->name), "%s", w[1]);
			found = 1;
		}
		/* later lines with the same name add addresses (by name) */
		for (int i = 1; i < n; i++)
			host_add_alias(h, w[i]);
		host_add_addr(h, ia.s_addr);
		if (!name)
			break;
	}
	fclose(f);
builtin:
	/* a system with no /etc/hosts still knows localhost */
	if (!found && (name ? !strcasecmp(name, "localhost")
			    : addr == htonl(INADDR_LOOPBACK))) {
		memset(h, 0, sizeof(*h));
		strcpy(h->name, "localhost");
		host_add_addr(h, htonl(INADDR_LOOPBACK));
		found = 1;
	}
	return found;
}

/* ---- DNS ---- */

struct resconf {
	struct in_addr ns[3];
	int nns;
	char search[6][128];
	int nsearch;
	int ndots;
};

static void res_read(struct resconf *rc)
{
	FILE *f = fopen("/etc/resolv.conf", "r");
	char line[512], *w[8];
	int n;

	memset(rc, 0, sizeof(*rc));
	rc->ndots = 1;
	if (f) {
		while (fgets(line, sizeof(line), f)) {
			if ((n = words(line, w, 8)) < 2)
				continue;
			if (!strcmp(w[0], "nameserver") && rc->nns < 3 &&
			    inet_pton(AF_INET, w[1], &rc->ns[rc->nns]) == 1)
				rc->nns++;
			else if (!strcmp(w[0], "domain") || !strcmp(w[0], "search")) {
				rc->nsearch = 0;
				for (int i = 1; i < n && rc->nsearch < 6; i++)
					if (strcmp(w[i], "."))
						snprintf(rc->search[rc->nsearch++],
							 sizeof(rc->search[0]), "%s", w[i]);
			} else if (!strcmp(w[0], "options"))
				for (int i = 1; i < n; i++)
					if (!strncmp(w[i], "ndots:", 6))
						rc->ndots = atoi(w[i] + 6);
		}
		fclose(f);
	}
	if (!rc->nns) {
		inet_pton(AF_INET, "127.0.0.1", &rc->ns[0]);
		rc->nns = 1;
	}
}

/* a name as DNS labels; the length, or -1 */
static int put_name(uint8_t *p, int room, const char *name)
{
	int n = 0;

	while (*name) {
		const char *dot = strchr(name, '.');
		int l = dot ? dot - name : (int)strlen(name);
		if (l == 0 || l > 63 || n + l + 2 > room)
			return -1;
		p[n++] = l;
		memcpy(p + n, name, l);
		n += l;
		name += l;
		if (*name == '.')
			name++;
	}
	if (n + 1 > room)
		return -1;
	p[n++] = 0;
	return n;
}

/* read a (compressed) name at off into out; the offset after it, or -1 */
static int get_name(const uint8_t *m, int len, int off, char *out, int room)
{
	int n = 0, jumped = 0, after = -1, hops = 0;

	for (;;) {
		if (off >= len)
			return -1;
		int l = m[off];
		if ((l & 0xc0) == 0xc0) {
			if (off + 1 >= len || ++hops > 32)
				return -1;
			if (!jumped)
				after = off + 2;
			jumped = 1;
			off = (l & 0x3f) << 8 | m[off + 1];
			continue;
		}
		off++;
		if (!l)
			break;
		if (off + l > len)
			return -1;
		if (n && n < room - 1)
			out[n++] = '.';
		for (int i = 0; i < l && n < room - 1; i++)
			out[n++] = m[off + i];
		off += l;
	}
	out[n] = 0;
	return jumped ? after : off;
}

volatile int nd_interrupted;     /* a ^C stops a lookup */

/* one query to the servers; the answer's length, 0 none, -1 interrupted */
static int dns_ask(struct resconf *rc, const char *name, int qtype,
		   uint8_t *ans, int anslen)
{
	uint8_t q[512];
	int qn, fd, id = (getpid() ^ time(NULL) ^ rand()) & 0xffff;

	memset(q, 0, 12);
	q[0] = id >> 8, q[1] = id;
	q[2] = 0x01;                            /* RD */
	q[5] = 1;                               /* QDCOUNT */
	if ((qn = put_name(q + 12, sizeof(q) - 16, name)) < 0)
		return 0;
	qn += 12;
	q[qn++] = 0, q[qn++] = qtype;
	q[qn++] = 0, q[qn++] = 1;               /* IN */
	fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return 0;
	for (int attempt = 0; attempt < 2; attempt++)
		for (int s = 0; s < rc->nns; s++) {
			struct sockaddr_in sa = { .sin_family = AF_INET,
				.sin_port = htons(53), .sin_addr = rc->ns[s] };
			struct pollfd p = { fd, POLLIN, 0 };
			if (sendto(fd, q, qn, 0, (struct sockaddr *)&sa, sizeof(sa)) < 0)
				continue;
			for (;;) {
				int r = poll(&p, 1, 2000 + attempt * 2000);
				if (r < 0 && errno == EINTR) {
					close(fd);
					nd_interrupted = 1;
					return -1;
				}
				if (r <= 0)
					break;
				int n = recv(fd, ans, anslen, 0);
				if (n >= 12 && ans[0] == q[0] && ans[1] == q[1] &&
				    (ans[2] & 0x80)) {
					close(fd);
					return n;
				}
			}
		}
	close(fd);
	return 0;
}

/*
 * Ask for name's A (type 1) or PTR (12) records.  Fills h from the
 * answer: A records' addresses, the CNAME chain's last name as the
 * official name (the others as aliases), a PTR's target as the name.
 * Returns 0 or an h_errno.
 */
static int dns_query(struct resconf *rc, const char *name, int qtype,
		     struct nd_host *h)
{
	uint8_t m[1500];
	char rn[256], tn[256];
	int len = dns_ask(rc, name, qtype, m, sizeof(m)), off, an, rcode;

	if (len < 0)
		return ND_TRY_AGAIN;
	if (len == 0)
		return ND_TRY_AGAIN;
	rcode = m[3] & 15;
	if (rcode == 3)
		return ND_HOST_NOT_FOUND;
	if (rcode)
		return rcode == 2 ? ND_TRY_AGAIN : ND_NO_RECOVERY;
	an = m[6] << 8 | m[7];
	off = 12;
	for (int i = 0, qd = m[4] << 8 | m[5]; i < qd; i++) {
		if ((off = get_name(m, len, off, rn, sizeof(rn))) < 0)
			return ND_NO_RECOVERY;
		off += 4;
	}
	memset(h, 0, sizeof(*h));
	snprintf(h->name, sizeof(h->name), "%s", name);
	for (int i = 0; i < an; i++) {
		int type, rdlen;
		if ((off = get_name(m, len, off, rn, sizeof(rn))) < 0 ||
		    off + 10 > len)
			break;
		type = m[off] << 8 | m[off + 1];
		rdlen = m[off + 8] << 8 | m[off + 9];
		off += 10;
		if (off + rdlen > len)
			break;
		if (type == 5) {                /* CNAME */
			if (get_name(m, len, off, tn, sizeof(tn)) > 0) {
				host_add_alias(h, h->name);
				snprintf(h->name, sizeof(h->name), "%s", tn);
			}
		} else if (type == 1 && qtype == 1 && rdlen == 4) {
			uint32_t a;
			memcpy(&a, m + off, 4);
			host_add_addr(h, a);
		} else if (type == 12 && qtype == 12) {
			if (get_name(m, len, off, tn, sizeof(tn)) > 0) {
				if (h->naddr++ == 0)    /* the first is the name */
					snprintf(h->name, sizeof(h->name), "%s", tn);
				else
					host_add_alias(h, tn);
			}
		}
		off += rdlen;
	}
	if (qtype == 1 && !h->naddr)
		return ND_NO_DATA;
	if (qtype == 12 && !h->naddr)
		return ND_NO_DATA;
	return 0;
}

int nd_gethostbyname(const char *name, struct nd_host *h)
{
	struct resconf rc;
	struct in_addr ia;
	char fq[512];
	int r = ND_HOST_NOT_FOUND, dots = 0;

	nd_interrupted = 0;
	if (!name || !*name)
		return ND_HOST_NOT_FOUND;
	/* a dotted address names itself, as BSD's resolver does */
	if (inet_pton(AF_INET, name, &ia) == 1) {
		memset(h, 0, sizeof(*h));
		snprintf(h->name, sizeof(h->name), "%s", name);
		h->addr[h->naddr++] = ia.s_addr;
		return 0;
	}
	if (hosts_file(name, 0, h))
		return 0;
	res_read(&rc);
	for (const char *p = name; *p; p++)
		dots += *p == '.';
	if (name[strlen(name) - 1] == '.') {
		snprintf(fq, sizeof(fq), "%s", name);
		fq[strlen(fq) - 1] = 0;
		return dns_query(&rc, fq, 1, h);
	}
	if (dots >= rc.ndots) {
		r = dns_query(&rc, name, 1, h);
		if (!r || r == ND_TRY_AGAIN)
			return r;
	}
	for (int i = 0; i < rc.nsearch; i++) {
		int s;
		snprintf(fq, sizeof(fq), "%s.%s", name, rc.search[i]);
		s = dns_query(&rc, fq, 1, h);
		if (!s)
			return 0;
		if (s == ND_TRY_AGAIN && nd_interrupted)
			return s;
	}
	if (dots < rc.ndots)
		r = dns_query(&rc, name, 1, h);
	return r;
}

int nd_gethostbyaddr(uint32_t addr, struct nd_host *h)
{
	struct resconf rc;
	char q[64];
	const uint8_t *b = (const uint8_t *)&addr;
	int r;

	nd_interrupted = 0;
	if (hosts_file(NULL, addr, h))
		return 0;
	res_read(&rc);
	snprintf(q, sizeof(q), "%u.%u.%u.%u.in-addr.arpa", b[3], b[2], b[1], b[0]);
	r = dns_query(&rc, q, 12, h);
	if (r)
		return r;
	h->naddr = 1;
	h->addr[0] = addr;
	return 0;
}
