#include "socks.h"
#include "socket_wrap.h"

static int ss_fd_set_init(struct ss_fd_set **fd_set)
{
	*fd_set = malloc(sizeof(struct ss_fd_set));

	if (*fd_set == NULL)
		return -1;
	FD_ZERO(&(*fd_set)->rfds);
	FD_ZERO(&(*fd_set)->wfds);
	return 0;
}

static int ss_fd_set_add_fd(struct ss_fd_set *fd_set, int fd, int mask)
{
	if (mask & AE_READABLE)
		FD_SET(fd, &fd_set->rfds);
	if (mask & AE_WRITABLE)
		FD_SET(fd, &fd_set->wfds);
	return 0;
}

static void ss_fd_set_del_fd(struct ss_fd_set *fd_set, int fd, int mask)
{
	if (mask & AE_READABLE)
		FD_CLR(fd, &fd_set->rfds);
	if (mask & AE_WRITABLE)
		FD_CLR(fd, &fd_set->wfds);
}

struct ss_server_ctx *ss_create_server(uint16_t port)
{
	struct ss_server_ctx *server;

	server = calloc(1, sizeof(typeof(*server)));
	if (server == NULL)
		return NULL;
	server->buf = buf_create(4096);
	if (server->buf == NULL)
		DIE("buf_create failed");
	server->sock_fd = create_server_socket(port);
	if (server->sock_fd < 0)
		DIE("create_server_socket failed!");
	server->fd_mask = AE_READABLE;
	server->max_fd = server->sock_fd;
	if (ss_fd_set_init(&server->ss_allfd_set) < 0)
		DIE("ss_fd_set_init failed!");
	if (ss_fd_set_add_fd(server->ss_allfd_set,
				server->sock_fd, AE_READABLE) < 0)
		DIE("ss_fd_set_add_fd failed!");
	server->fd_state = calloc(1, sizeof(*server->fd_state));
	if (server->fd_state == NULL)
		DIE("calloc failed!");
	server->conn = calloc(1, sizeof(*server->conn));
	if (server->conn == NULL)
		DIE("calloc failed!");
	INIT_LIST_HEAD(&server->conn->list);
	server->time_event_list = calloc(1, sizeof(*server->time_event_list));
	if (server->time_event_list == NULL)
		DIE("calloc failed!");
	INIT_LIST_HEAD(&server->time_event_list->list);
	return server;
}

struct ss_conn_ctx *ss_server_add_conn(struct ss_server_ctx *s, int conn_fd,
		int mask, struct conn_info *conn_info, struct io_event *event)
{
	struct ss_conn_ctx *new_conn;

	new_conn = calloc(1, sizeof(*new_conn));
	if (new_conn == NULL)
		return NULL;
	new_conn->conn_fd = conn_fd;
	new_conn->server_entry = s;
	new_conn->fd_mask = mask;
	new_conn->msg= buf_create(4096);
	if (new_conn->msg == NULL)
		DIE("buf_create failed");
	new_conn->ss_conn_state = OPENING;
	if (conn_info) {
		strncpy(new_conn->ss_conn_info.ip, conn_info->ip,
				sizeof(new_conn->ss_conn_info.ip) - 1);
		new_conn->ss_conn_info.ip[sizeof(new_conn->ss_conn_info.ip) - 1]
									= '\0';
		new_conn->ss_conn_info.port = conn_info->port;
	}
	if (event)
		memcpy(&new_conn->io_proc, event, sizeof(*event));
	list_add(&new_conn->list, &s->conn->list);
	s->conn_count++;
	s->max_fd = (conn_fd > s->max_fd) ? conn_fd : s->max_fd;
	if (ss_fd_set_add_fd(s->ss_allfd_set, conn_fd, mask) < 0)
		DIE("ss_fd_set_add_fd failed!");
	return new_conn;
}

void ss_server_del_conn(struct ss_server_ctx *s, struct ss_conn_ctx *conn)
{
	ss_fd_set_del_fd(s->ss_allfd_set, conn->conn_fd, AE_READABLE);
	s->conn_count--;
	list_del(&conn->list);
	buf_release(conn->msg);
	close(conn->conn_fd);
	conn->ss_conn_state = CLOSED;
	if (conn->data != NULL)
		free(conn->data);
	free(conn);
}

int ss_handshake_handle(struct ss_conn_ctx *conn)
{
	ssize_t ret;
	struct buf *buf = conn->server_entry->buf;

	ret = recv(conn->conn_fd, buf->data, 262, 0);
	if (ret <= 0)
		goto err;
	if (buf->data[0] != 0x05)
		goto err;
	/* TODO: 检查客户端支持的认证机制 */
	buf->data[0] = 0x05;
	buf->data[1] = 0x0; /* NO AUTHENTICATION REQUIRED */
	ret = send(conn->conn_fd, buf->data, 2, 0);
	if (ret != 2)
		goto err;
	conn->ss_conn_state = CONNECTING;
	return 0;
err:
	debug_print("handshake failed!");
	ss_server_del_conn(conn->server_entry, conn);
	return -1;
}

int ss_msg_handle(struct ss_conn_ctx *conn,
		void (*func)(struct ss_conn_ctx *conn))
{
	/* TODO */
	struct buf *buf = conn->server_entry->buf;
	ssize_t ret;

	ret = recv(conn->conn_fd, buf->data, 4, 0);
	if (ret != 4) {
		ss_server_del_conn(conn->server_entry, conn);
		return -1;
	}
	if (buf->data[0] != 0x05 || buf->data[2] != 0) {
		ss_server_del_conn(conn->server_entry, conn);
		return -1;
	}
	if (buf->data[1] != 0x01) {
		debug_print("only support CONNECT CMD now -_-");
		ss_server_del_conn(conn->server_entry, conn);
		return -1;
	}
	switch (buf->data[3]) { /* ATYP */
	int s_addr = inet_aton("0.0.0.0", NULL);
	uint32_t us_addr = htonl(s_addr);
	case 0x01: /* IPv4 */
		ret = recv(conn->conn_fd, buf->data + 4, 6, 0);
		if (ret != 6) {
			ss_server_del_conn(conn->server_entry, conn);
			return -1;
		}
		buf->data[0] = 0x5;
		buf->data[1] = 0x0;
		buf->data[2] = 0x0;
		buf->data[3] = 0x1;
		memcpy(&buf->data[4], &us_addr, 4);
		buf->data[4 + 4] = 0x19;
		buf->data[4 + 5] = 0x19;
		buf->used = 10;
		ret = send(conn->conn_fd, buf->data, buf->used, 0);
		if (ret != buf->used) {
			debug_print("send return %d", (int)ret);
			return -1;
		}
		break;
	case 0x03: /* FQDN */
		ret = recv(conn->conn_fd, buf->data + 4, 1, 0);
		if (ret != 1) {
			ss_server_del_conn(conn->server_entry, conn);
			return -1;
		}
		uint8_t url_length = buf->data[4];
		ret = recv(conn->conn_fd, buf->data + 5, url_length + 2, 0);
		if (ret != url_length + 2) {
			ss_server_del_conn(conn->server_entry, conn);
			return -1;
		}
		buf->data[0] = 0x5;
		buf->data[1] = 0x0;
		buf->data[2] = 0x0;
		buf->data[3] = 0x1;
		memcpy(&buf->data[4], &us_addr, 4);
		buf->data[4 + 4] = 0x19;
		buf->data[4 + 5] = 0x19;
		buf->used = 10;
		ret = send(conn->conn_fd, buf->data, buf->used, 0);
		if (ret != buf->used) {
			debug_print("send return %d", (int)ret);
			return -1;
		}
		break;
	case 0x04: /* IPv6 */
		break;
	default:
		debug_print("err ATYP: %x", buf->data[3]);
		ss_server_del_conn(conn->server_entry, conn);
		return -1;
	}
	func(conn);
	return 0;
}

int ss_send_msg_conn(struct ss_conn_ctx *conn, int msg_type)
{
	/* TODO */
	int ret;
	struct buf *buf = conn->server_entry->buf;

	strcpy((char *)buf->data, "hello");
	ret = send(conn->conn_fd, buf->data, 5, 0);
	if (ret != 5) {
		debug_print("send return %d, buf->used is %d", ret, 5);
		return -1;
	}
	return 0;
}

static inline void gettime(long *seconds, long *milliseconds)
{
	struct timeval tv;

	gettimeofday(&tv, NULL);
	*seconds = tv.tv_sec;
	*milliseconds = tv.tv_usec / 1000;
}

static int get_nearest_timer(const struct ss_server_ctx *s, struct timeval *tv)
{
	struct time_event *te;
	struct time_event *nearest = NULL;
	long now_sec, now_ms;

	list_for_each_entry(te, &s->time_event_list->list, list) {
		if (!nearest || te->when_sec < nearest->when_sec
		    || (te->when_sec == nearest->when_sec
		    && te->when_ms < nearest->when_ms))
			nearest = te;
	}
	if (nearest == NULL)
		return -1;
	gettime(&now_sec, &now_ms);
	tv->tv_sec = nearest->when_sec - now_sec;
	if (nearest->when_ms < now_ms) {
		tv->tv_usec = ((nearest->when_ms + 1000) - now_ms) * 1000;
		tv->tv_sec -= 1;
	} else
		tv->tv_usec = (nearest->when_ms - now_ms) * 1000;
	if (tv->tv_sec < 0)
		tv->tv_sec = 0;
	if (tv->tv_usec < 0)
		tv->tv_usec = 0;
	return 0;
}

static int ss_poll(struct ss_server_ctx *server)
{
	int numevents = 0;
	int retval;
	struct ss_fd_set *set = server->ss_allfd_set;
	struct ss_conn_ctx *conn;
	struct timeval nearest_tv;
	struct timeval *tv;

	memcpy(&set->_rfds, &set->rfds, sizeof(fd_set));
	memcpy(&set->_wfds, &set->wfds, sizeof(fd_set));
	retval = get_nearest_timer(server, &nearest_tv);
	tv = retval < 0 ? NULL : &nearest_tv;
	retval = select(server->max_fd + 1, &set->_rfds, &set->_wfds, NULL, tv);
	if (retval > 0) {
		if (FD_ISSET(server->sock_fd, &set->_rfds)) {
			server->io_proc.mask |= AE_READABLE;
			server->fd_state[numevents].type = SS_SERVER_CTX;
			server->fd_state[numevents++].ctx_ptr = server;
		}
		list_for_each_entry(conn, &server->conn->list, list) {
			if (conn->fd_mask & AE_READABLE
			    && FD_ISSET(conn->conn_fd, &set->_rfds)) {
				conn->io_proc.mask |= AE_READABLE;
				server->fd_state[numevents].type = SS_CONN_CTX;
				server->fd_state[numevents++].ctx_ptr = conn;
			}
		}
	}
	return numevents;
}

static inline int is_timeup(const struct time_event *te)
{
	long now_sec, now_ms;

	gettime(&now_sec, &now_ms);
	return now_sec > te->when_sec ||
		(now_sec == te->when_sec
		&& now_ms >= te->when_ms);
}

static void addmillisecondstonow(uint64_t milliseconds, long *sec, long *ms)
{
	long cur_sec, cur_ms, when_sec, when_ms;

	gettime(&cur_sec, &cur_ms);
	when_sec = cur_sec + milliseconds / 1000;
	when_ms = cur_ms + milliseconds % 1000;
	if (when_ms >= 1000) {
		when_sec ++;
		when_ms -= 1000;
	}
	*sec = when_sec;
	*ms = when_ms;
}

static inline int proc_time_event(struct ss_server_ctx *s)
{
	int processed = 0;
	struct time_event *te;
	struct list_head *pos, *q;
	int ret;

	list_for_each_safe(pos, q, &s->time_event_list->list) {
		te = list_entry(pos, struct time_event, list);
		ret = is_timeup(te);
		if (ret) {
			ret = te->timeproc(s, te->id, te->para);
			if (ret != AE_NOMORE)
				addmillisecondstonow(ret, &te->when_sec,
							&te->when_ms);
			else
				ss_server_del_time_event(te);
			processed++;
		}
	}
	return processed;
}

void ss_loop(struct ss_server_ctx *server)
{
	int numevents;
	struct io_event *event;
	int fd;
	int i;

	while (1) {
		numevents = ss_poll(server);
		for (i = 0; i < numevents; i++) {
			if (server->fd_state[i].type == SS_SERVER_CTX) {
				/* accept */
				event = &server->io_proc;
				fd = server->sock_fd;
			} else if (server->fd_state[i].type == SS_CONN_CTX) {
				/* recv */
				event = &((struct ss_conn_ctx *)server->
						fd_state[i].ctx_ptr)->io_proc;
				fd = ((struct ss_conn_ctx *)server->
						fd_state[i].ctx_ptr)->conn_fd;
			}
			if (event->mask & AE_READABLE &&
					event->rfileproc != NULL)
				event->rfileproc(server->fd_state[i].ctx_ptr,
						fd, event->para, event->mask);
		}
		proc_time_event(server);
	}
}

void ss_release_server(struct ss_server_ctx *ss_server)
{
	free(ss_server->time_event_list);
	free(ss_server->conn);
	free(ss_server->fd_state);
	free(ss_server->ss_allfd_set);
	buf_release(ss_server->buf);
	free(ss_server);
}

int ss_server_add_time_event(struct ss_server_ctx *s, uint64_t ms,
		ss_timeproc *proc, void *para)
{
	struct time_event *new_te;

	new_te = calloc(1, sizeof(*new_te));
	if (new_te == NULL)
		return -1;
	new_te->id = s->time_event_next_id++;
	addmillisecondstonow(ms, &new_te->when_sec, &new_te->when_ms);
	new_te->timeproc = proc;
	new_te->para = para;
	list_add(&new_te->list, &s->time_event_list->list);
	return 0;
}

void ss_server_del_time_event(struct time_event *te)
{
	assert(te);
	list_del(&te->list);
	free(te);
	te = NULL;
}
