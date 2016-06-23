/*
 * Copyright (c) 2016 Gilles Chehade <gilles@poolp.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "includes.h"

#include <sys/types.h>

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>
#include <limits.h>
#include <stdlib.h>
#include <unistd.h>

#include "smtpd-defines.h"
#include "smtpd-api.h"
#include "log.h"
#include "iobuf.h"
#include "ioev.h"

#define RSPAMD_HOST "127.0.0.1"
#define RSPAMD_PORT "11333"


struct session {
	uint64_t	id;

	struct iobuf	iobuf;
	struct io	io;
	
	char	       *ip;
	char	       *hostname;
	char	       *helo;

	struct tx {
		FILE   *fp;
		char	*line;
		int		eom;
		int		rspamd_done;

		char	       *from;
		char	       *rcpt;
	} tx;

};

struct sockaddr_storage	ss;

static struct session  *rspamd_session_init(uint64_t);
static void		rspamd_session_reset(struct session *);
static void		rspamd_session_free(struct session *);
static void		rspamd_io(struct io *, int);
static void		rspamd_send_query(struct session *);
	
/* XXX
 * this needs to be handled differently, but lets focus on the filter for now
 */
static void
rspamd_resolve(const char *h, const char *p)
{
	struct addrinfo hints, *addresses, *ai;
	int fd, r;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = PF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;
	if ((r = getaddrinfo(h, p, &hints, &addresses)))
		fatalx("resolve: getaddrinfo %s", gai_strerror(r));
	for (ai = addresses; ai; ai = ai->ai_next) {
		if ((fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol)) == -1)
			continue;
		if (connect(fd, ai->ai_addr, ai->ai_addrlen) == -1) {
			close(fd);
			continue;
		}
		close(fd);
		memmove(&ss, ai->ai_addr, ai->ai_addrlen);
		break;
	}
	freeaddrinfo(addresses);
	if (!ai)
		fatalx("resolve: failed");
}

static struct session *
rspamd_session_init(uint64_t id)
{
	struct session	*rs;

	rs = xcalloc(1, sizeof *rs, "on_connect");
	rs->id = id;
	return rs;
}

static void
rspamd_session_reset(struct session *rs)
{
	free(rs->tx.from);
	free(rs->tx.rcpt);

	filter_api_datahold_close(rs->id);

	rs->tx.eom = 0;
	rs->tx.rspamd_done = 0;
}

static void
rspamd_session_free(struct session *rs)
{
	iobuf_clear(&rs->iobuf);
	io_clear(&rs->io);

	rspamd_session_reset(rs);

	free(rs->ip);
	free(rs->hostname);
	free(rs->helo);
	free(rs);
}

static int
rspamd_connect(struct session *rs)
{
	iobuf_xinit(&rs->iobuf, LINE_MAX, LINE_MAX, "on_eom");
	io_init(&rs->io, -1, rs, rspamd_io, &rs->iobuf);
	if (io_connect(&rs->io, (struct sockaddr *)&ss, NULL) == -1)
		return 0;
	return 1;
}

static void
rspamd_connected(struct session *rs)
{
	filter_api_accept(rs->id);
}

static void
rspamd_error(struct session *rs)
{
	/* XXX */
	filter_api_accept(rs->id);
}

static void
rspamd_send_query(struct session *rs)
{
	iobuf_xfqueue(&rs->iobuf, "io",
	    "POST /check HTTP/1.0\r\n"
	    "IP: %s\r\n"
	    "Helo: %s\r\n"
	    "Hostname: %s\r\n"
	    "From: %s\r\n"
	    "Rcpt: %s\r\n"
	    "Pass: all\r\n"
	    "Transfer-Encoding: chunked\r\n\r\n",
	    rs->ip,
	    rs->helo,
	    rs->hostname,
	    rs->tx.from,
	    rs->tx.rcpt);
	io_reload(&rs->io);
}

static void
rspamd_send_chunk(struct session *rs, const char *line)
{
	iobuf_xfqueue(&rs->iobuf, "io", "%x\r\n%s\r\n\r\n",
	    strlen(line)+2, line);
	io_reload(&rs->io);
}

static void
rspamd_send_eom(struct session *rs)
{
	iobuf_xfqueue(&rs->iobuf, "io", "0\r\n\r\n");
	rs->tx.eom = 1;
	io_reload(&rs->io);
}

static void
rspamd_response(struct session *rs)
{
	char	       *line;

	while ((line = iobuf_getline(&rs->iobuf, NULL)))
		log_debug("debug: DATAIN: [%s]", line);
	if (iobuf_len(&rs->iobuf) != 0) {
		log_debug("debug: DATAIN: [%.*s]",
		    (int)iobuf_len(&rs->iobuf),
		    iobuf_data(&rs->iobuf));		
	}
	iobuf_normalize(&rs->iobuf);

	rs->tx.rspamd_done = 1;
}

static void
smtpd_stream_back(uint64_t id, FILE *fp, void *arg)
{
	struct session *rs = arg;
	size_t		sz;
	ssize_t		len;

	len = getline(&rs->tx.line, &sz, fp);

	/* XXX */
	if (len == -1) {
		filter_api_accept(rs->id);
		return;
	}

	if (strcmp(rs->tx.line, "\n") == 0)
		filter_api_writeln(rs->id, "X-MangeDes: Bites");

	rs->tx.line[len-1] = 0;
	log_debug("debug: STREAM BACK: [%s]", rs->tx.line);
	filter_api_writeln(rs->id, rs->tx.line);
}

static void
rspamd_io(struct io *io, int evt)
{
	struct session *rs = io->arg;

	switch (evt) {
	case IO_CONNECTED:
		rspamd_connected(rs);
		rspamd_send_query(rs);
		io_set_write(io);
		break;

	case IO_LOWAT:
		/* we've hit EOM and no more data, toggle to read */
		if (rs->tx.eom)
			io_set_read(io);
		break;

	case IO_DATAIN:
		rspamd_response(rs);
		if (rs->tx.rspamd_done) {
			log_debug("debug: ####### WILL STREAM BACK");
			filter_api_datahold_start(rs->id);
			io_set_write(io);
		}
		break;

	case IO_DISCONNECTED:
		log_debug("debug: DISCONNECT");
		rspamd_session_free(rs);
		break;
	case IO_TIMEOUT:
		log_debug("debug: TIMEOUT");
		break;
	case IO_ERROR:
		log_debug("debug: ERROR");
		break;
	default:
		log_debug("debug: WTF");
		break;
	}
	return;
}





/* callbacks */

static int
on_connect(uint64_t id, struct filter_connect *conn)
{
	struct session	*rs;
	const char	*ip;

	rs = rspamd_session_init(id);

	//ip = filter_api_sockaddr_to_text((struct sockaddr *)&conn->local);
	ip = "127.0.0.1";
	rs->ip = xstrdup(ip, "on_connect");
	rs->hostname = xstrdup(conn->hostname, "on_connect");
	filter_api_set_udata(id, rs);
	return filter_api_accept(id);
}

static int
on_helo(uint64_t id, const char *helo)
{
	struct session	*rs = filter_api_get_udata(id);

	rs->helo = xstrdup(helo, "on_helo");
	return filter_api_accept(id);
}

static int
on_mail(uint64_t id, struct mailaddr *mail)
{
	struct session	*rs = filter_api_get_udata(id);
	const char	*address;

	address = filter_api_mailaddr_to_text(mail);
	rs->tx.from = xstrdup(address, "on_mail");
	return filter_api_accept(id);
}

static int
on_rcpt(uint64_t id, struct mailaddr *rcpt)
{
	struct session	*rs = filter_api_get_udata(id);
	const char	*address;

	address = filter_api_mailaddr_to_text(rcpt);
	rs->tx.rcpt = xstrdup(address, "on_rcpt");
	return filter_api_accept(id);
}

static int
on_data(uint64_t id)
{
	struct session *rs = filter_api_get_udata(id);

	rs->tx.fp = filter_api_datahold_open(id, smtpd_stream_back, rs);
	if (rs->tx.fp == NULL)
		return filter_api_accept(id); /* XXX */

	if (! rspamd_connect(rs))
		return filter_api_accept(id); /* XXX */

	return 1;
}

static void
on_dataline(uint64_t id, const char *line)
{
	struct session	*rs = filter_api_get_udata(id);

	/* XXX - tempfail here */
	fprintf(rs->tx.fp, "%s\n", line);
	rspamd_send_chunk(rs, line);
}

static int
on_eom(uint64_t id, size_t size)
{
	struct session	*rs = filter_api_get_udata(id);

	rspamd_send_eom(rs);
}

static void
on_commit(uint64_t id)
{
	struct session	*rs = filter_api_get_udata(id);

	rspamd_session_reset(rs);
}

static void
on_rollback(uint64_t id)
{
	struct session	*rs = filter_api_get_udata(id);

	rspamd_session_reset(rs);
}

static void
on_disconnect(uint64_t id)
{
	rspamd_session_free((struct session *)filter_api_get_udata(id));
}

int
main(int argc, char **argv)
{
	int ch, C = 0, d = 0, v = 0;
	const char *l = NULL;
	char *c = NULL, *h = RSPAMD_HOST, *p = RSPAMD_PORT, *s = NULL;

	log_init(1);

	while ((ch = getopt(argc, argv, "dh:l:p:s:v")) != -1) {
		switch (ch) {
		case 'C':
			C = 1;
			break;
		case 'c':
			c = optarg;
			break;
		case 'd':
			d = 1;
			break;
		case 'h':
			h = optarg;
			break;
		case 'l':
			l = optarg;
			break;
		case 'p':
			p = optarg;
			break;
		case 's':
			s = optarg;
			break;
		case 'v':
			v |= TRACE_DEBUG;
			break;
		default:
			log_warnx("warn: bad option");
			return 1;
			/* NOTREACHED */
		}
	}
	argc -= optind;
	argv += optind;

	if (c)
		c = strip(c);
	if (h)
		h = strip(h);
	if (p)
		p = strip(p);

	log_init(d);
	log_verbose(v);

	log_debug("debug: starting...");

	rspamd_resolve(h, p);

	filter_api_on_connect(on_connect);
	filter_api_on_helo(on_helo);
	filter_api_on_mail(on_mail);
	filter_api_on_rcpt(on_rcpt);
	filter_api_on_data(on_data);
	filter_api_on_dataline(on_dataline);
	filter_api_on_eom(on_eom);
	filter_api_on_commit(on_commit);
	filter_api_on_rollback(on_rollback);
	filter_api_on_disconnect(on_disconnect);

	/*
	if (c)
		filter_api_set_chroot(c);
	if (C)
		filter_api_no_chroot();
	*/
	filter_api_no_chroot();

	filter_api_loop();
	log_debug("debug: exiting");

	return 1;
}