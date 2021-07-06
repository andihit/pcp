/*
 * Copyright (c) 2018-2019 Red Hat.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public
 * License for more details.
 */
#include <sys/time.h>
#include <sys/resource.h>

#include "server.h"
#include "uv_callback.h"
#include <assert.h>

static uv_signal_t	sighup, sigint, sigterm;

static struct {
	const char	*group;
	char		*path;
} server_metrics[] = {
	{ .group = NULL },		/* METRICS_NOTUSED */
	{ .group = "server" },		/* METRICS_SERVER */
	{ .group = "redis" },		/* METRICS_REDIS */
	{ .group = "http" },		/* METRICS_HTTP */
	{ .group = "pcp" },		/* METRICS_PCP */
	{ .group = "discover" },	/* METRICS_DISCOVER */
	{ .group = "series" },		/* METRICS_SERIES */
	{ .group = "webgroup" },	/* METRICS_WEBGROUP */
	{ .group = "search" },          /* METRICS_SEARCH */
};

void
proxylog(pmLogLevel level, sds message, void *arg)
{
    struct proxy	*proxy = (struct proxy *)arg;
    const char		*state = proxy->slots ? "" : "- DISCONNECTED - ";
    int			priority;

    switch (level) {
    case PMLOG_TRACE:
    case PMLOG_DEBUG:
	priority = LOG_DEBUG;
	break;
    case PMLOG_INFO:
	priority = LOG_INFO;
	break;
    case PMLOG_WARNING:
	priority = LOG_WARNING;
	break;
    case PMLOG_CORRUPT:
	priority = LOG_CRIT;
	break;
    default:
	priority = LOG_ERR;
	break;
    }
    pmNotifyErr(priority, "%s%s", state, message);
}

mmv_registry_t *
proxymetrics(struct proxy *proxy, enum proxy_registry prid)
{
    mmv_stats_flags_t	flags = MMV_FLAG_PROCESS;
    mmv_registry_t	*registry;
    char		*file, path[MAXPATHLEN];
    int			sep = pmPathSeparator();

    if (prid >= NUM_REGISTRY || prid <= METRICS_NOTUSED)
	return NULL;

    if (proxy->metrics[prid] != NULL)	/* already setup */
	return proxy->metrics[prid];

    pmsprintf(path, sizeof(path), "%s%cpmproxy%c%s",
	    pmGetConfig("PCP_TMP_DIR"), sep, sep, server_metrics[prid].group);
    if ((file = strdup(path)) == NULL)
	return NULL;

    if (prid == METRICS_SERVER)
	flags |= MMV_FLAG_NOPREFIX;
    if ((registry = mmv_stats_registry(file, prid, flags)) != NULL)
	server_metrics[prid].path = file;
    else
	free(file);
    proxy->metrics[prid] = registry;
    return registry;
}

void
proxymetrics_close(struct proxy *proxy, enum proxy_registry prid)
{
    if (prid >= NUM_REGISTRY || prid <= METRICS_NOTUSED)
	return;

    if (proxy->metrics[prid] != NULL) {
	mmv_stats_free(proxy->metrics[prid]);
	proxy->metrics[prid] = NULL;
    }

    if (server_metrics[prid].path != NULL) {
	free(server_metrics[prid].path);
	server_metrics[prid].path = NULL;
    }
}

static void
server_metrics_refresh(void *arg)
{
    struct proxy	*proxy = (struct proxy *)arg;
    struct rusage	usage;
    double		user, sys;
    pmAtomValue		*value;

    if (getrusage(RUSAGE_SELF, &usage) < 0)
    	return;
    user = 1000.0 * pmtimevalToReal(&usage.ru_utime);
    sys = 1000.0 * pmtimevalToReal(&usage.ru_stime);

    if ((value = mmv_lookup_value_desc(proxy->metrics_handle, "mem.maxrss", NULL)) != NULL)
	mmv_set_value(proxy->metrics_handle, value, usage.ru_maxrss);
    if ((value = mmv_lookup_value_desc(proxy->metrics_handle, "cpu.user", NULL)) != NULL)
	mmv_set_value(proxy->metrics_handle, value, user);
    if ((value = mmv_lookup_value_desc(proxy->metrics_handle, "cpu.sys", NULL)) != NULL)
	mmv_set_value(proxy->metrics_handle, value, sys);
    if ((value = mmv_lookup_value_desc(proxy->metrics_handle, "cpu.total", NULL)) != NULL)
	mmv_set_value(proxy->metrics_handle, value, user + sys);
}

static void
server_metrics_init(struct proxy *proxy)
{
    mmv_registry_t	*registry;
    pmAtomValue		*value;
    pmInDom		noindom = MMV_INDOM_NULL;
    pmUnits		nounits = MMV_UNITS(0,0,0,0,0,0);
    pmUnits		units_kbytes = MMV_UNITS(1, 0, 0, PM_SPACE_KBYTE, 0, 0);
    pmUnits		units_msec = MMV_UNITS(0, 1, 0, 0, PM_TIME_MSEC, 0);
    pid_t		pid = getpid();
    char		buffer[64];

    if ((registry = proxy->metrics[METRICS_SERVER]) == NULL)
	return;

    mmv_stats_add_metric(registry, "pid", SERVER_PID,
		MMV_TYPE_U32, MMV_SEM_DISCRETE, nounits, noindom,
		"pmproxy PID",
		"PID for the current pmproxy invocation");

    pmsprintf(buffer, sizeof(buffer), "%u", pid);
    mmv_stats_add_metric_label(registry, SERVER_PID,
		"pid", buffer, MMV_NUMBER_TYPE, 0);

    mmv_stats_add_metric(registry, "mem.maxrss", SERVER_MEM_MAXRSS,
	MMV_TYPE_U64, MMV_SEM_INSTANT, units_kbytes, noindom,
	"pmproxy maximum RSS",
	"pmproxy process maximum resident set memory size");

    mmv_stats_add_metric(registry, "cpu.user", SERVER_CPU_USER,
	MMV_TYPE_U64, MMV_SEM_COUNTER, units_msec, noindom,
	"pmproxy user CPU",
	"pmproxy process user CPU time counter");

    mmv_stats_add_metric(registry, "cpu.sys", SERVER_CPU_SYS,
	MMV_TYPE_U64, MMV_SEM_COUNTER, units_msec, noindom,
	"pmproxy system CPU",
	"pmproxy process system CPU time counter");

    mmv_stats_add_metric(registry, "cpu.total", SERVER_CPU_TOTAL,
	MMV_TYPE_U64, MMV_SEM_COUNTER, units_msec, noindom,
	"pmproxy system + user CPU",
	"pmproxy process system + user CPU time");

    if ((proxy->metrics_handle = mmv_stats_start(registry)) == NULL) {
	fprintf(stderr, "%s: instrumentation disabled\n", pmGetProgname());
	return;
    }

    /* never need to refresh pid, so just set it once */
    if ((value = mmv_lookup_value_desc(proxy->metrics_handle, "pid", NULL)) != NULL)
	mmv_set_value(proxy->metrics_handle, value, pid);
}

static struct proxy *
server_init(int portcount, const char *localpath)
{
    struct server	*servers;
    struct proxy	*proxy;
    int			count;

    if ((proxy = calloc(1, sizeof(struct proxy))) == NULL) {
	fprintf(stderr, "%s: out-of-memory in proxy server setup\n",
			pmGetProgname());
	return NULL;
    }
    uv_mutex_init(&proxy->mutex);

    count = portcount + (*localpath ? 1 : 0);
    if (count) {
	/* allocate space for maximum listen port data structures */
	if ((servers = calloc(count, sizeof(struct server))) == NULL) {
	    fprintf(stderr, "%s: out-of-memory allocating for %d ports\n",
			    pmGetProgname(), count);
	    free(proxy);
	    return NULL;
	}
	proxy->servers = servers;
    } else {
	fprintf(stderr, "%s: no ports or local paths specified\n",
			pmGetProgname());
	free(proxy);
	return NULL;
    }

    proxy->config = config;

    proxymetrics(proxy, METRICS_SERVER);
    server_metrics_init(proxy);
    pmSeriesRegisterTimer(proxy, server_metrics_refresh);

    proxy->events = uv_default_loop();

    return proxy;
}

static void
signal_handler(uv_signal_t *sighandle, int signum)
{
    uv_handle_t		*handle = (uv_handle_t *)sighandle;
    struct proxy	*proxy = (struct proxy *)handle->data;
    uv_loop_t		*loop = proxy->events;

    if (signum == SIGHUP)
	return;
    pmNotifyErr(LOG_INFO, "pmproxy caught %s\n",
		signum == SIGINT ? "SIGINT" : "SIGTERM");
    uv_signal_stop(&sigterm);
    uv_signal_stop(&sigint);
    uv_signal_stop(&sighup);
    uv_close((uv_handle_t *)&sigterm, NULL);
    uv_close((uv_handle_t *)&sigint, NULL);
    uv_close((uv_handle_t *)&sighup, NULL);
    uv_stop(loop);
}

static void
signal_init(struct proxy *proxy)
{
    uv_loop_t		*loop = proxy->events;

    signal(SIGPIPE, SIG_IGN);

    uv_signal_init(loop, &sighup);
    uv_signal_init(loop, &sigint);
    uv_signal_init(loop, &sigterm);
    sighup.data = sigint.data = sigterm.data = (void *)proxy;
    uv_signal_start(&sighup, signal_handler, SIGHUP);
    uv_signal_start(&sigint, signal_handler, SIGINT);
    uv_signal_start(&sigterm, signal_handler, SIGTERM);
}

void
on_buffer_alloc(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf)
{
    if (pmDebugOptions.desperate)
	fprintf(stderr, "%s: handle %p buffer allocation of %lld bytes\n",
			"on_buffer_alloc", handle, (long long)suggested_size);

    if ((buf->base = sdsnewlen(SDS_NOINIT, suggested_size)) != NULL)
	buf->len = suggested_size;
    else
	buf->len = 0;
}

static void
on_client_close(uv_handle_t *handle)
{
    struct client	*client = (struct client *)handle;

    if (pmDebugOptions.context | pmDebugOptions.desperate)
	fprintf(stderr, "%s: client %p connection closed\n",
			"on_client_close", client);

    client_put(client);
}

void
client_get(struct client *client)
{
    uv_mutex_lock(&client->mutex);
    assert(client->refcount);
    client->refcount++;
    uv_mutex_unlock(&client->mutex);
}

void
client_put(struct client *client)
{
    unsigned int	refcount;
    struct proxy	*proxy = client->proxy;

    uv_mutex_lock(&client->mutex);
    assert(client->refcount);
    refcount = --client->refcount;
    uv_mutex_unlock(&client->mutex);

    if (refcount == 0) {
	/* remove client from the doubly-linked list */
	uv_mutex_lock(&proxy->mutex);
	if (client->next != NULL)
	    client->next->prev = client->prev;
	*client->prev = client->next;
	uv_mutex_unlock(&proxy->mutex);

	if (client->protocol & STREAM_PCP)
	    on_pcp_client_close(client);
	if (client->protocol & STREAM_HTTP)
	    on_http_client_close(client);
	if (client->protocol & STREAM_REDIS)
	    on_redis_client_close(client);
	if (client->protocol & STREAM_SECURE)
	    on_secure_client_close(client);

	memset(client, 0, sizeof(*client));
	free(client);
    }
}

int
client_is_closed(struct client *client)
{
    return !client->opened;
}

void
client_close(struct client *client)
{
    if (client->opened == 1) {
	client->opened = 0;
	uv_close((uv_handle_t *)client, on_client_close);
    }
}

void
on_client_write(uv_write_t *writer, int status)
{
    struct client	*client = (struct client *)writer->data;
    stream_write_baton	*request = (stream_write_baton *)writer;

    if (pmDebugOptions.af)
	fprintf(stderr, "%s: completed write [sts=%d] to client %p\n",
			"on_client_write", status, client);

    if (status == 0) {
	if (client->protocol & STREAM_SECURE)
	    on_secure_client_write(client);
	if (client->protocol & STREAM_PCP)
	    on_pcp_client_write(client);
	else if (client->protocol & STREAM_HTTP)
	    on_http_client_write(client);
	else if (client->protocol & STREAM_REDIS)
	    on_redis_client_write(client);
    }

    sdsfree(request->buffer[0].base);
    request->buffer[0].base = NULL;
    if (request->buffer[1].base) {	/* optional second buffer */
	sdsfree(request->buffer[1].base);
	request->buffer[1].base = NULL;
    }
    free(request);

    if (status == 0)
	return;

    if (pmDebugOptions.af)
	fprintf(stderr, "%s: %s\n", "on_client_write", uv_strerror(status));
    client_close(client);
}

void *
on_write_callback(uv_callback_t *handle, void *data)
{
    stream_write_baton	*request = (stream_write_baton *)data;
    struct client	*client = (struct client *)request->writer.data;
    int			sts;

    if (pmDebugOptions.af)
	fprintf(stderr, "%s: client=%p\n", "on_write_callback", client);

    if (client->stream.secure == 0) {
	sts = uv_write(&request->writer, (uv_stream_t *)&client->stream,
		 &request->buffer[0], request->nbuffers, request->callback);
	if (sts != 0)
	    fprintf(stderr, "%s: ERROR uv_write failed\n", "on_write_callback");
    } else
	secure_client_write(client, request);
    (void)handle;
    return 0;
}

void
client_write(struct client *client, sds buffer, sds suffix)
{
    stream_write_baton	*request;
    struct proxy	*proxy = client->proxy;
    unsigned int	nbuffers = 0;

    if (client_is_closed(client))
	return;

    if ((request = calloc(1, sizeof(stream_write_baton))) != NULL) {
	if (pmDebugOptions.af)
	    fprintf(stderr, "%s: sending %ld bytes [0] to client %p\n",
			"client_write", (long)sdslen(buffer), client);
	request->buffer[nbuffers++] = uv_buf_init(buffer, sdslen(buffer));
	if (suffix != NULL) {
	    if (pmDebugOptions.af)
		fprintf(stderr, "%s: sending %ld bytes [1] to client %p\n",
			"client_write", (long)sdslen(suffix), client);
	    request->buffer[nbuffers++] = uv_buf_init(suffix, sdslen(suffix));
	}
	request->nbuffers = nbuffers;
	request->writer.data = client;
	request->callback = on_client_write;

	uv_callback_fire(&proxy->write_callbacks, request, NULL);
    } else {
	client_close(client);
    }
}

static stream_protocol
client_protocol(int key)
{
    switch (key) {
    case 'p':	/* PCP pmproxy */
	return STREAM_PCP;
    case 'G':	/* HTTP GET */
    case 'H':	/* HTTP HEAD */
    case 'P':	/* HTTP POST, PUT, PATCH */
    case 'D':	/* HTTP DELETE */
    case 'T':	/* HTTP TRACE */
    case 'O':	/* HTTP OPTIONS */
    case 'C':	/* HTTP CONNECT */
	return STREAM_HTTP;
    case '-':	/* RESP error */
    case '+':	/* RESP status */
    case ':':	/* RESP integer */
    case ',':	/* RESP double */
    case '$':	/* RESP string */
    case '*':	/* RESP array */
    case '#':	/* RESP bool */
    case '%':	/* RESP map */
    case '~':	/* RESP set */
	return STREAM_REDIS;
    case 0x14:	/* TLS ChangeCipherSpec */
    case 0x15:	/* TLS Alert */
    case 0x16:	/* TLS Handshake */
    case 0x17:	/* TLS Application */
    case 0x18:	/* TLS Heartbeat */
	return STREAM_SECURE;
    default:
	break;
    }
    return STREAM_UNKNOWN;
}

void
on_protocol_read(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf)
{
    struct proxy	*proxy = (struct proxy *)stream->data;
    struct client	*client = (struct client *)stream;

    if (nread < 0)
	return;

    if ((client->protocol & (STREAM_PCP|STREAM_HTTP|STREAM_REDIS)) == 0)
	client->protocol |= client_protocol(*buf->base);

    if (client->protocol & STREAM_PCP)
	on_pcp_client_read(proxy, client, nread, buf);
    else if (client->protocol & STREAM_HTTP)
	on_http_client_read(proxy, client, nread, buf);
    else if (client->protocol & STREAM_REDIS)
	on_redis_client_read(proxy, client, nread, buf);
    else {
	if (pmDebugOptions.af)
	    fprintf(stderr, "%s: unknown protocol key '%c' (0x%x)"
			    " - disconnecting client %p\n", "on_protocol_read",
		    *buf->base, (unsigned int)*buf->base, proxy);
	client_close(client);
    }
}

static void
on_client_read(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf)
{
    struct proxy	*proxy = (struct proxy *)stream->data;
    struct client	*client = (struct client *)stream;

    if (nread > 0) {
	if (client->protocol == STREAM_UNKNOWN)
	    client->protocol |= client_protocol(*buf->base);
	if (client->protocol & STREAM_SECURE)
	    on_secure_client_read(proxy, client, nread, buf);
	else
	    on_protocol_read(stream, nread, buf);
    } else if (nread < 0) {
	if (pmDebugOptions.af)
	    fprintf(stderr, "%s: read error %ld "
		    "- disconnecting client %p\n", "on_client_read",
		    (long)nread, client);
	client_close(client);
    }
    sdsfree(buf->base);
}

static void
on_client_connection(uv_stream_t *stream, int status)
{
    struct proxy	*proxy = (struct proxy *)stream->data;
    struct client	*client;
    uv_handle_t		*handle;

    if (status != 0) {
	fprintf(stderr, "%s: client connection failed: %s\n",
			pmGetProgname(), uv_strerror(status));
	return;
    }

    if ((client = calloc(1, sizeof(*client))) == NULL) {
	fprintf(stderr, "%s: out-of-memory for new client\n",
			pmGetProgname());
	return;
    }
    if (pmDebugOptions.context | pmDebugOptions.af)
	fprintf(stderr, "%s: accept new client %p\n",
			"on_client_connection", client);

    /* prepare per-client lock for reference counting */
    uv_mutex_init(&client->mutex);
    client->refcount = 1;
    client->opened = 1;

    status = uv_tcp_init(proxy->events, &client->stream.u.tcp);
    if (status != 0) {
	fprintf(stderr, "%s: client tcp init failed: %s\n",
			pmGetProgname(), uv_strerror(status));
	client_put(client);
	return;
    }

    status = uv_accept(stream, (uv_stream_t *)&client->stream.u.tcp);
    if (status != 0) {
	fprintf(stderr, "%s: client tcp init failed: %s\n",
			pmGetProgname(), uv_strerror(status));
	client_put(client);
	return;
    }
    handle = (uv_handle_t *)&client->stream.u.tcp;
    handle->data = (void *)proxy;
    client->proxy = proxy;

    /* insert client into doubly-linked list at the head */
    uv_mutex_lock(&proxy->mutex);
    if ((client->next = proxy->first) != NULL)
	proxy->first->prev = &client->next;
    proxy->first = client;
    client->prev = &proxy->first;
    uv_mutex_unlock(&proxy->mutex);

    status = uv_read_start((uv_stream_t *)&client->stream.u.tcp,
			    on_buffer_alloc, on_client_read);
    if (status != 0) {
	fprintf(stderr, "%s: client read start failed: %s\n",
			pmGetProgname(), uv_strerror(status));
	client_close(client);
    }
}

static int
open_request_port(struct proxy *proxy, struct server *server, stream_family family,
		const struct sockaddr *addr, int port, int maxpending)
{
    struct stream	*stream = &server->stream;
    uv_handle_t		*handle;
    sds			option;
    int			sts, flags = 0, keepalive = 45;

    if ((option = pmIniFileLookup(proxy->config, "pmproxy", "keepalive")))
	keepalive = atoi(option);

    stream->family = family;
    if (family == STREAM_TCP6)
	flags = UV_TCP_IPV6ONLY;
    stream->port = port;

    uv_tcp_init(proxy->events, &stream->u.tcp);
    handle = (uv_handle_t *)&stream->u.tcp;
    handle->data = (void *)proxy;

    uv_tcp_bind(&stream->u.tcp, addr, flags);
    uv_tcp_nodelay(&stream->u.tcp, 1);
    uv_tcp_keepalive(&stream->u.tcp, keepalive > 0, keepalive);

    sts = uv_listen((uv_stream_t *)&stream->u.tcp, maxpending, on_client_connection);
    if (sts != 0) {
	fprintf(stderr, "%s: socket listen error %s\n",
			pmGetProgname(), uv_strerror(sts));
	uv_close(handle, NULL);
	return -ENOTCONN;
    }
    stream->active = 1;
    if (__pmServerHasFeature(PM_SERVER_FEATURE_DISCOVERY))
	server->presence = __pmServerAdvertisePresence(PM_SERVER_PROXY_SPEC, port);
    return 0;
}

static int
open_request_local(struct proxy *proxy, struct server *server,
		const char *name, int maxpending)
{
    uv_handle_t		*handle;
    struct stream	*stream = &server->stream;
    int			sts;

    stream->family = STREAM_LOCAL;

    uv_pipe_init(proxy->events, &stream->u.local, 0);
    handle = (uv_handle_t *)&stream->u.local;
    handle->data = (void *)proxy;
    uv_pipe_bind(&stream->u.local, name);
#ifdef HAVE_UV_PIPE_CHMOD
    uv_pipe_chmod(&stream->u.local, UV_READABLE | UV_WRITABLE);
#endif

    sts = uv_listen((uv_stream_t *)&stream->u.local, maxpending, on_client_connection);
    if (sts != 0) {
	fprintf(stderr, "%s: local listen error %s\n",
			pmGetProgname(), uv_strerror(sts));
	uv_close(handle, NULL);
        return -ENOTCONN;
    }
    stream->active = 1;
    __pmServerSetFeature(PM_SERVER_FEATURE_UNIX_DOMAIN);
    return 0;
}

static void
setup_default_local_path(char *localpath, size_t localpathlen)
{
    char		*envstr;

    if ((envstr = getenv("PMPROXY_SOCKET")) != NULL)
	pmsprintf(localpath, localpathlen, "%s", envstr);
    else
	pmsprintf(localpath, localpathlen, "%s%c" "pmproxy.socket",
			pmGetConfig("PCP_RUN_DIR"), pmPathSeparator());
}

typedef struct proxyaddr {
    __pmSockAddr	*addr;
    const char		*address;
    int			port;
} proxyaddr;

static void *
open_request_ports(char *localpath, size_t localpathlen, int maxpending)
{
    int			inaddr, total, count, port, sts, i, n;
    int			with_ipv6 = strcmp(pmGetAPIConfig("ipv6"), "true") == 0;
    const char		*address;
    __pmSockAddr	*addr;
    struct proxyaddr	*addrlist;
    const struct sockaddr *sockaddr;
    stream_family	family;
    struct server	*server;
    struct proxy	*proxy;

    if (localpath[0] == '\0')
	setup_default_local_path(localpath, localpathlen);

    if ((sts = total = __pmServerSetupRequestPorts()) < 0)
	return NULL;

    /* allow for both IPv6 and IPv4 addresses for each port */
    if ((addrlist = calloc(total * 2, sizeof(proxyaddr))) == NULL)
	return NULL;

    /* fill in sockaddr structs for subsequent listen calls */
    for (i = n = 0; i < total; i++) {
	__pmServerGetRequestPort(i, &address, &port);
	addrlist[n].address = address;
	addrlist[n].port = port;

	if (address != NULL &&
	    strcmp(address, "INADDR_ANY") != 0 &&
	    strcmp(address, "INADDR_LOOPBACK") != 0) {
	    addr = __pmStringToSockAddr(address);
	    if (__pmSockAddrGetFamily(addr) == AF_UNSPEC)
		__pmSockAddrFree(addr);
	    else {
		__pmSockAddrSetPort(addr, port);
		addrlist[n++].addr = addr;
		continue;
	    }
	}

	/* address unspecified - create both ipv4 and ipv6 entries */
	if (address == NULL || strcmp(address, "INADDR_ANY") == 0)
	    inaddr = INADDR_ANY;
	else if (strcmp(address, "INADDR_LOOPBACK") == 0)
	    inaddr = INADDR_LOOPBACK;
	else
	    continue;

	addrlist[n].addr = __pmSockAddrAlloc();
	__pmSockAddrInit(addrlist[n].addr, AF_INET, inaddr, port);
	n++;

	if (!with_ipv6)
	     continue;

	addrlist[n].addr = __pmSockAddrAlloc();
	__pmSockAddrInit(addrlist[n].addr, AF_INET6, inaddr, port);
	n++;
    }
    total = n;

    if ((proxy = server_init(total, localpath)) == NULL)
	goto fail;

    signal_init(proxy);

    count = n = 0;
    if (*localpath) {
	unlink(localpath);
	server = &proxy->servers[n++];
	server->stream.address = localpath;
	if (open_request_local(proxy, server, localpath, maxpending) == 0)
	    count++;
    }

    for (i = 0; i < total; i++) {
	sockaddr = (const struct sockaddr *)addrlist[i].addr;
	family = __pmSockAddrGetFamily(addrlist[i].addr) == AF_INET ?
					STREAM_TCP4 : STREAM_TCP6;
	port = __pmSockAddrGetPort(addrlist[i].addr);
	server = &proxy->servers[n++];
	server->stream.address = addrlist[i].address;
	if (open_request_port(proxy, server, family, sockaddr, port, maxpending) == 0)
	    count++;
	__pmSockAddrFree(addrlist[i].addr);
    }
    free(addrlist);

    if (count == 0) {
	pmNotifyErr(LOG_ERR, "%s: can't open any request ports, exiting\n",
		pmGetProgname());
	free(proxy);
	return NULL;
    }
    proxy->nservers = n;
    return proxy;

fail:
    for (i = 0; i < n; i++)
	__pmSockAddrFree(addrlist[i].addr);
    free(addrlist);
    return NULL;
}

static void
close_proxy(struct proxy *proxy)
{
    close_pcp_module(proxy);
    close_http_module(proxy);
    close_redis_module(proxy);
    close_secure_module(proxy);
}

static void
shutdown_ports(void *arg)
{
    struct proxy	*proxy = (struct proxy *)arg;
    struct server	*server;
    struct stream	*stream;
    int			i;

    for (i = 0; i < proxy->nservers; i++) {
	server = &proxy->servers[i];
	stream = &server->stream;
	if (stream->active == 0)
	    continue;
	if (stream->family == STREAM_LOCAL) {
	    uv_close((uv_handle_t *)&stream->u.local, NULL);
	    unlink(stream->address);
	} else {
	    uv_close((uv_handle_t *)&stream->u.tcp, NULL);
	    if (server->presence)
		__pmServerUnadvertisePresence(server->presence);
	}
    }
    proxy->nservers = 0;

    close_proxy(proxy);

    if (proxy->config) {
	pmIniFileFree(proxy->config);
	proxy->config = NULL;
    }

    uv_loop_close(proxy->events);
    proxymetrics_close(proxy, METRICS_SERVER);
    pmSeriesDeregisterTimers();

    free(proxy->servers);
    proxy->servers = NULL;
}

static void
dump_request_ports(FILE *output, void *arg)
{
    struct proxy	*proxy = (struct proxy *)arg;
    struct stream	*stream;
    uv_os_fd_t		uv_fd;
    int			i, fd;

    fprintf(output, "%s request port(s):\n"
		"  sts fd   port  family address\n"
		"  === ==== ===== ====== =======\n", pmGetProgname());

    for (i = 0; i < proxy->nservers; i++) {
	stream = &proxy->servers[i].stream;
	fd = (uv_fileno((uv_handle_t *)stream, &uv_fd) < 0) ? -1 : (int)uv_fd;
	if (stream->family == STREAM_LOCAL)
	    fprintf(output, "  %-3s %4d %5s %-6s %s\n",
		    stream->active ? "ok" : "err", fd, "",
		    "unix", stream->address);
	else
	    fprintf(output, "  %-3s %4d %5d %-6s %s\n",
		    stream->active ? "ok" : "err", fd, stream->port,
		    stream->family == STREAM_TCP4 ? "inet" : "ipv6",
		    stream->address ? stream->address : "INADDR_ANY");
    }
}

/*
 * Initial setup for each of the major sub-systems modules,
 * which is achieved via a timer that expires immediately.
 * Once any connections are established (async) modules are
 * again informed via their individual setup routines.
 */
static void
setup_proxy(uv_timer_t *arg)
{
    uv_handle_t		*handle = (uv_handle_t *)arg;
    struct proxy	*proxy = (struct proxy *)handle->data;

    setup_secure_module(proxy);
    setup_redis_module(proxy);
    setup_http_module(proxy);
    setup_pcp_module(proxy);
}

static void
prepare_proxy(uv_prepare_t *arg)
{
    uv_handle_t		*handle = (uv_handle_t *)arg;
    struct proxy	*proxy = (struct proxy *)handle->data;

    flush_secure_module(proxy);
}

static void
check_proxy(uv_check_t *arg)
{
    uv_handle_t		*handle = (uv_handle_t *)arg;
    struct proxy	*proxy = (struct proxy *)handle->data;

    flush_secure_module(proxy);
}

static void
main_loop(void *arg)
{
    struct proxy	*proxy = (struct proxy *)arg;
    uv_timer_t		initial_io;
    uv_prepare_t	before_io;
    uv_check_t		after_io;
    uv_handle_t		*handle;

    uv_timer_init(proxy->events, &initial_io);
    handle = (uv_handle_t *)&initial_io;
    handle->data = (void *)proxy;
    uv_timer_start(&initial_io, setup_proxy, 0, 0);

    uv_prepare_init(proxy->events, &before_io);
    handle = (uv_handle_t *)&before_io;
    handle->data = (void *)proxy;
    uv_prepare_start(&before_io, prepare_proxy);

    uv_check_init(proxy->events, &after_io);
    handle = (uv_handle_t *)&after_io;
    handle->data = (void *)proxy;
    uv_check_start(&after_io, check_proxy);

    uv_callback_init(proxy->events, &proxy->write_callbacks,
		    on_write_callback, UV_DEFAULT);

    uv_run(proxy->events, UV_RUN_DEFAULT);
}

struct pmproxy libuv_pmproxy = {
    .openports	= open_request_ports,
    .dumpports	= dump_request_ports,
    .shutdown	= shutdown_ports,
    .loop 	= main_loop,
};
