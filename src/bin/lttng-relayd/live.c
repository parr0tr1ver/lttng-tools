/*
 * Copyright (C) 2013 - Julien Desfossez <jdesfossez@efficios.com>
 *                      David Goulet <dgoulet@efficios.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 2 only,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#define _GNU_SOURCE
#include <getopt.h>
#include <grp.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <inttypes.h>
#include <urcu/futex.h>
#include <urcu/uatomic.h>
#include <unistd.h>
#include <fcntl.h>
#include <config.h>

#include <lttng/lttng.h>
#include <common/common.h>
#include <common/compat/poll.h>
#include <common/compat/socket.h>
#include <common/defaults.h>
#include <common/futex.h>
#include <common/index/index.h>
#include <common/sessiond-comm/sessiond-comm.h>
#include <common/sessiond-comm/inet.h>
#include <common/sessiond-comm/relayd.h>
#include <common/uri.h>
#include <common/utils.h>

#include "cmd.h"
#include "live.h"
#include "lttng-relayd.h"
#include "utils.h"
#include "health-relayd.h"
#include "testpoint.h"
#include "viewer-stream.h"
#include "stream.h"
#include "session.h"
#include "ctf-trace.h"

static struct lttng_uri *live_uri;

/*
 * This pipe is used to inform the worker thread that a command is queued and
 * ready to be processed.
 */
static int live_relay_cmd_pipe[2] = { -1, -1 };

/* Shared between threads */
static int live_dispatch_thread_exit;

static pthread_t live_listener_thread;
static pthread_t live_dispatcher_thread;
static pthread_t live_worker_thread;

/*
 * Relay command queue.
 *
 * The live_thread_listener and live_thread_dispatcher communicate with this
 * queue.
 */
static struct relay_cmd_queue viewer_cmd_queue;

static uint64_t last_relay_viewer_session_id;

/*
 * Cleanup the daemon
 */
static
void cleanup(void)
{
	DBG("Cleaning up");

	free(live_uri);
}

/*
 * Receive a request buffer using a given socket, destination allocated buffer
 * of length size.
 *
 * Return the size of the received message or else a negative value on error
 * with errno being set by recvmsg() syscall.
 */
static
ssize_t recv_request(struct lttcomm_sock *sock, void *buf, size_t size)
{
	ssize_t ret;

	assert(sock);
	assert(buf);

	ret = sock->ops->recvmsg(sock, buf, size, 0);
	if (ret < 0 || ret != size) {
		if (ret == 0) {
			/* Orderly shutdown. Not necessary to print an error. */
			DBG("Socket %d did an orderly shutdown", sock->fd);
		} else {
			ERR("Relay failed to receive request.");
		}
		ret = -1;
	}

	return ret;
}

/*
 * Send a response buffer using a given socket, source allocated buffer of
 * length size.
 *
 * Return the size of the sent message or else a negative value on error with
 * errno being set by sendmsg() syscall.
 */
static
ssize_t send_response(struct lttcomm_sock *sock, void *buf, size_t size)
{
	ssize_t ret;

	assert(sock);
	assert(buf);

	ret = sock->ops->sendmsg(sock, buf, size, 0);
	if (ret < 0) {
		ERR("Relayd failed to send response.");
	}

	return ret;
}

/*
 * Atomically check if new streams got added in the session since the last
 * check and reset the flag to 0.
 *
 * Returns 1 if new streams got added, 0 if nothing changed, a negative value
 * on error.
 */
static
int check_new_streams(uint64_t session_id, struct lttng_ht *sessions_ht)
{
	int ret;
	unsigned long current_val;
	struct relay_session *session;

	assert(sessions_ht);

	session = session_find_by_id(sessions_ht, session_id);
	if (!session) {
		DBG("Relay session %" PRIu64 " not found", session_id);
		ret = -1;
		goto error;
	}

	current_val = uatomic_cmpxchg(&session->new_streams, 1, 0);
	ret = current_val;

error:
	return ret;
}

/*
 * Send viewer streams to the given socket. The ignore_sent_flag indicates if
 * this function should ignore the sent flag or not.
 *
 * Return 0 on success or else a negative value.
 */
static
ssize_t send_viewer_streams(struct lttcomm_sock *sock,
		struct relay_session *session, unsigned int ignore_sent_flag)
{
	ssize_t ret;
	struct lttng_viewer_stream send_stream;
	struct lttng_ht_iter iter;
	struct relay_viewer_stream *vstream;

	assert(session);

	rcu_read_lock();

	cds_lfht_for_each_entry(viewer_streams_ht->ht, &iter.iter, vstream,
			stream_n.node) {
		struct ctf_trace *ctf_trace;

		health_code_update();

		/* Ignore if not the same session. */
		if (vstream->session_id != session->id ||
				(!ignore_sent_flag && vstream->sent_flag)) {
			continue;
		}

		ctf_trace = ctf_trace_find_by_path(session->ctf_traces_ht,
				vstream->path_name);
		assert(ctf_trace);

		send_stream.id = htobe64(vstream->stream_handle);
		send_stream.ctf_trace_id = htobe64(ctf_trace->id);
		send_stream.metadata_flag = htobe32(vstream->metadata_flag);
		strncpy(send_stream.path_name, vstream->path_name,
				sizeof(send_stream.path_name));
		strncpy(send_stream.channel_name, vstream->channel_name,
				sizeof(send_stream.channel_name));

		DBG("Sending stream %" PRIu64 " to viewer", vstream->stream_handle);
		ret = send_response(sock, &send_stream, sizeof(send_stream));
		if (ret < 0) {
			goto end_unlock;
		}
		vstream->sent_flag = 1;
	}

	ret = 0;

end_unlock:
	rcu_read_unlock();
	return ret;
}

/*
 * Create every viewer stream possible for the given session with the seek
 * type. Three counters *can* be return which are in order the total amount of
 * viewer stream of the session, the number of unsent stream and the number of
 * stream created. Those counters can be NULL and thus will be ignored.
 *
 * Return 0 on success or else a negative value.
 */
static
int make_viewer_streams(struct relay_session *session,
		enum lttng_viewer_seek seek_t, uint32_t *nb_total, uint32_t *nb_unsent,
		uint32_t *nb_created)
{
	int ret;
	struct lttng_ht_iter iter;
	struct ctf_trace *ctf_trace;

	assert(session);

	/*
	 * This is to make sure we create viewer streams for a full received
	 * channel. For instance, if we have 8 streams for a channel that are
	 * concurrently being flagged ready, we can end up creating just a subset
	 * of the 8 streams (the ones that are flagged). This lock avoids this
	 * limbo state.
	 */
	pthread_mutex_lock(&session->viewer_ready_lock);

	/*
	 * Create viewer streams for relay streams that are ready to be used for a
	 * the given session id only.
	 */
	rcu_read_lock();
	cds_lfht_for_each_entry(session->ctf_traces_ht->ht, &iter.iter, ctf_trace,
			node.node) {
		struct relay_stream *stream;

		health_code_update();

		if (ctf_trace->invalid_flag) {
			continue;
		}

		cds_list_for_each_entry(stream, &ctf_trace->stream_list, trace_list) {
			struct relay_viewer_stream *vstream;

			if (!stream->viewer_ready) {
				continue;
			}

			vstream = viewer_stream_find_by_id(stream->stream_handle);
			if (!vstream) {
				vstream = viewer_stream_create(stream, seek_t, ctf_trace);
				if (!vstream) {
					ret = -1;
					goto error_unlock;
				}
				/* Acquire reference to ctf_trace. */
				ctf_trace_get_ref(ctf_trace);

				if (nb_created) {
					/* Update number of created stream counter. */
					(*nb_created)++;
				}
			} else if (!vstream->sent_flag && nb_unsent) {
				/* Update number of unsent stream counter. */
				(*nb_unsent)++;
			}
			/* Update number of total stream counter. */
			if (nb_total) {
				(*nb_total)++;
			}
		}
	}

	ret = 0;

error_unlock:
	rcu_read_unlock();
	pthread_mutex_unlock(&session->viewer_ready_lock);
	return ret;
}

/*
 * Write to writable pipe used to notify a thread.
 */
static
int notify_thread_pipe(int wpipe)
{
	ssize_t ret;

	ret = lttng_write(wpipe, "!", 1);
	if (ret < 1) {
		PERROR("write poll pipe");
	}

	return (int) ret;
}

/*
 * Stop all threads by closing the thread quit pipe.
 */
static
void stop_threads(void)
{
	int ret;

	/* Stopping all threads */
	DBG("Terminating all live threads");
	ret = notify_thread_pipe(live_conn_pipe[1]);
	if (ret < 0) {
		ERR("write error on thread quit pipe");
	}

	/* Dispatch thread */
	CMM_STORE_SHARED(live_dispatch_thread_exit, 1);
	futex_nto1_wake(&viewer_cmd_queue.futex);
}

/*
 * Create a poll set with O_CLOEXEC and add the thread quit pipe to the set.
 */
static
int create_thread_poll_set(struct lttng_poll_event *events, int size)
{
	int ret;

	if (events == NULL || size == 0) {
		ret = -1;
		goto error;
	}

	ret = lttng_poll_create(events, size, LTTNG_CLOEXEC);
	if (ret < 0) {
		goto error;
	}

	/* Add quit pipe */
	ret = lttng_poll_add(events, live_conn_pipe[0], LPOLLIN | LPOLLERR);
	if (ret < 0) {
		goto error;
	}

	return 0;

error:
	return ret;
}

/*
 * Check if the thread quit pipe was triggered.
 *
 * Return 1 if it was triggered else 0;
 */
static
int check_live_conn_pipe(int fd, uint32_t events)
{
	if (fd == live_conn_pipe[0] && (events & LPOLLIN)) {
		return 1;
	}

	return 0;
}

/*
 * Create and init socket from uri.
 */
static
struct lttcomm_sock *init_socket(struct lttng_uri *uri)
{
	int ret;
	struct lttcomm_sock *sock = NULL;

	sock = lttcomm_alloc_sock_from_uri(uri);
	if (sock == NULL) {
		ERR("Allocating socket");
		goto error;
	}

	ret = lttcomm_create_sock(sock);
	if (ret < 0) {
		goto error;
	}
	DBG("Listening on sock %d for live", sock->fd);

	ret = sock->ops->bind(sock);
	if (ret < 0) {
		goto error;
	}

	ret = sock->ops->listen(sock, -1);
	if (ret < 0) {
		goto error;

	}

	return sock;

error:
	if (sock) {
		lttcomm_destroy_sock(sock);
	}
	return NULL;
}

/*
 * This thread manages the listening for new connections on the network
 */
static
void *thread_listener(void *data)
{
	int i, ret, pollfd, err = -1;
	int val = 1;
	uint32_t revents, nb_fd;
	struct lttng_poll_event events;
	struct lttcomm_sock *live_control_sock;

	DBG("[thread] Relay live listener started");

	health_register(health_relayd, HEALTH_RELAYD_TYPE_LIVE_LISTENER);

	health_code_update();

	live_control_sock = init_socket(live_uri);
	if (!live_control_sock) {
		goto error_sock_control;
	}

	/* Pass 2 as size here for the thread quit pipe and control sockets. */
	ret = create_thread_poll_set(&events, 2);
	if (ret < 0) {
		goto error_create_poll;
	}

	/* Add the control socket */
	ret = lttng_poll_add(&events, live_control_sock->fd, LPOLLIN | LPOLLRDHUP);
	if (ret < 0) {
		goto error_poll_add;
	}

	lttng_relay_notify_ready();

	if (testpoint(relayd_thread_live_listener)) {
		goto error_testpoint;
	}

	while (1) {
		health_code_update();

		DBG("Listener accepting live viewers connections");

restart:
		health_poll_entry();
		ret = lttng_poll_wait(&events, -1);
		health_poll_exit();
		if (ret < 0) {
			/*
			 * Restart interrupted system call.
			 */
			if (errno == EINTR) {
				goto restart;
			}
			goto error;
		}
		nb_fd = ret;

		DBG("Relay new viewer connection received");
		for (i = 0; i < nb_fd; i++) {
			health_code_update();

			/* Fetch once the poll data */
			revents = LTTNG_POLL_GETEV(&events, i);
			pollfd = LTTNG_POLL_GETFD(&events, i);

			/* Thread quit pipe has been closed. Killing thread. */
			ret = check_live_conn_pipe(pollfd, revents);
			if (ret) {
				err = 0;
				goto exit;
			}

			if (revents & (LPOLLERR | LPOLLHUP | LPOLLRDHUP)) {
				ERR("socket poll error");
				goto error;
			} else if (revents & LPOLLIN) {
				/*
				 * Get allocated in this thread, enqueued to a global queue,
				 * dequeued and freed in the worker thread.
				 */
				struct relay_command *relay_cmd;
				struct lttcomm_sock *newsock;

				relay_cmd = zmalloc(sizeof(*relay_cmd));
				if (!relay_cmd) {
					PERROR("relay command zmalloc");
					goto error;
				}

				assert(pollfd == live_control_sock->fd);
				newsock = live_control_sock->ops->accept(live_control_sock);
				if (!newsock) {
					PERROR("accepting control sock");
					free(relay_cmd);
					goto error;
				}
				DBG("Relay viewer connection accepted socket %d", newsock->fd);
				ret = setsockopt(newsock->fd, SOL_SOCKET, SO_REUSEADDR, &val,
						sizeof(int));
				if (ret < 0) {
					PERROR("setsockopt inet");
					lttcomm_destroy_sock(newsock);
					free(relay_cmd);
					goto error;
				}
				relay_cmd->sock = newsock;

				/*
				 * Lock free enqueue the request.
				 */
				cds_wfq_enqueue(&viewer_cmd_queue.queue, &relay_cmd->node);

				/*
				 * Wake the dispatch queue futex. Implicit memory
				 * barrier with the exchange in cds_wfq_enqueue.
				 */
				futex_nto1_wake(&viewer_cmd_queue.futex);
			}
		}
	}

exit:
error:
error_poll_add:
error_testpoint:
	lttng_poll_clean(&events);
error_create_poll:
	if (live_control_sock->fd >= 0) {
		ret = live_control_sock->ops->close(live_control_sock);
		if (ret) {
			PERROR("close");
		}
	}
	lttcomm_destroy_sock(live_control_sock);
error_sock_control:
	if (err) {
		health_error();
		DBG("Live viewer listener thread exited with error");
	}
	health_unregister(health_relayd);
	DBG("Live viewer listener thread cleanup complete");
	stop_threads();
	return NULL;
}

/*
 * This thread manages the dispatching of the requests to worker threads
 */
static
void *thread_dispatcher(void *data)
{
	int err = -1;
	ssize_t ret;
	struct cds_wfq_node *node;
	struct relay_command *relay_cmd = NULL;

	DBG("[thread] Live viewer relay dispatcher started");

	health_register(health_relayd, HEALTH_RELAYD_TYPE_LIVE_DISPATCHER);

	if (testpoint(relayd_thread_live_dispatcher)) {
		goto error_testpoint;
	}

	health_code_update();

	while (!CMM_LOAD_SHARED(live_dispatch_thread_exit)) {
		health_code_update();

		/* Atomically prepare the queue futex */
		futex_nto1_prepare(&viewer_cmd_queue.futex);

		do {
			health_code_update();

			/* Dequeue commands */
			node = cds_wfq_dequeue_blocking(&viewer_cmd_queue.queue);
			if (node == NULL) {
				DBG("Woken up but nothing in the live-viewer "
						"relay command queue");
				/* Continue thread execution */
				break;
			}

			relay_cmd = caa_container_of(node, struct relay_command, node);
			DBG("Dispatching viewer request waiting on sock %d",
					relay_cmd->sock->fd);

			/*
			 * Inform worker thread of the new request. This call is blocking
			 * so we can be assured that the data will be read at some point in
			 * time or wait to the end of the world :)
			 */
			ret = lttng_write(live_relay_cmd_pipe[1], relay_cmd,
					sizeof(*relay_cmd));
			free(relay_cmd);
			if (ret < sizeof(struct relay_command)) {
				PERROR("write cmd pipe");
				goto error;
			}
		} while (node != NULL);

		/* Futex wait on queue. Blocking call on futex() */
		health_poll_entry();
		futex_nto1_wait(&viewer_cmd_queue.futex);
		health_poll_exit();
	}

	/* Normal exit, no error */
	err = 0;

error:
error_testpoint:
	if (err) {
		health_error();
		ERR("Health error occurred in %s", __func__);
	}
	health_unregister(health_relayd);
	DBG("Live viewer dispatch thread dying");
	stop_threads();
	return NULL;
}

/*
 * Establish connection with the viewer and check the versions.
 *
 * Return 0 on success or else negative value.
 */
static
int viewer_connect(struct relay_command *cmd)
{
	int ret;
	struct lttng_viewer_connect reply, msg;

	assert(cmd);

	cmd->version_check_done = 1;

	health_code_update();

	DBG("Viewer is establishing a connection to the relayd.");

	ret = recv_request(cmd->sock, &msg, sizeof(msg));
	if (ret < 0) {
		goto end;
	}

	health_code_update();

	reply.major = RELAYD_VERSION_COMM_MAJOR;
	reply.minor = RELAYD_VERSION_COMM_MINOR;

	/* Major versions must be the same */
	if (reply.major != be32toh(msg.major)) {
		DBG("Incompatible major versions ([relayd] %u vs [client] %u)",
				reply.major, be32toh(msg.major));
		ret = -1;
		goto end;
	}

	cmd->major = reply.major;
	/* We adapt to the lowest compatible version */
	if (reply.minor <= be32toh(msg.minor)) {
		cmd->minor = reply.minor;
	} else {
		cmd->minor = be32toh(msg.minor);
	}

	if (be32toh(msg.type) == VIEWER_CLIENT_COMMAND) {
		cmd->type = RELAY_VIEWER_COMMAND;
	} else if (be32toh(msg.type) == VIEWER_CLIENT_NOTIFICATION) {
		cmd->type = RELAY_VIEWER_NOTIFICATION;
	} else {
		ERR("Unknown connection type : %u", be32toh(msg.type));
		ret = -1;
		goto end;
	}

	reply.major = htobe32(reply.major);
	reply.minor = htobe32(reply.minor);
	if (cmd->type == RELAY_VIEWER_COMMAND) {
		reply.viewer_session_id = htobe64(++last_relay_viewer_session_id);
	}

	health_code_update();

	ret = send_response(cmd->sock, &reply, sizeof(reply));
	if (ret < 0) {
		goto end;
	}

	health_code_update();

	DBG("Version check done using protocol %u.%u", cmd->major, cmd->minor);
	ret = 0;

end:
	return ret;
}

/*
 * Send the viewer the list of current sessions.
 *
 * Return 0 on success or else a negative value.
 */
static
int viewer_list_sessions(struct relay_command *cmd,
		struct lttng_ht *sessions_ht)
{
	int ret;
	struct lttng_viewer_list_sessions session_list;
	unsigned long count;
	long approx_before, approx_after;
	struct lttng_ht_iter iter;
	struct lttng_viewer_session send_session;
	struct relay_session *session;

	DBG("List sessions received");

	rcu_read_lock();
	cds_lfht_count_nodes(sessions_ht->ht, &approx_before, &count, &approx_after);
	session_list.sessions_count = htobe32(count);

	health_code_update();

	ret = send_response(cmd->sock, &session_list, sizeof(session_list));
	if (ret < 0) {
		goto end_unlock;
	}

	health_code_update();

	cds_lfht_for_each_entry(sessions_ht->ht, &iter.iter, session,
			session_n.node) {
		health_code_update();

		strncpy(send_session.session_name, session->session_name,
				sizeof(send_session.session_name));
		strncpy(send_session.hostname, session->hostname,
				sizeof(send_session.hostname));
		send_session.id = htobe64(session->id);
		send_session.live_timer = htobe32(session->live_timer);
		send_session.clients = htobe32(session->viewer_refcount);
		send_session.streams = htobe32(session->stream_count);

		health_code_update();

		ret = send_response(cmd->sock, &send_session, sizeof(send_session));
		if (ret < 0) {
			goto end_unlock;
		}
	}
	health_code_update();

	rcu_read_unlock();
	ret = 0;
	goto end;

end_unlock:
	rcu_read_unlock();

end:
	return ret;
}

/*
 * Send the viewer the list of current sessions.
 */
static
int viewer_get_new_streams(struct relay_command *cmd,
		struct lttng_ht *sessions_ht)
{
	int ret, send_streams = 0;
	uint32_t nb_created = 0, nb_unsent = 0, nb_streams = 0;
	struct lttng_viewer_new_streams_request request;
	struct lttng_viewer_new_streams_response response;
	struct relay_session *session;

	assert(cmd);
	assert(sessions_ht);

	DBG("Get new streams received");

	health_code_update();

	/* Receive the request from the connected client. */
	ret = recv_request(cmd->sock, &request, sizeof(request));
	if (ret < 0) {
		goto error;
	}

	health_code_update();

	rcu_read_lock();
	session = session_find_by_id(sessions_ht, be64toh(request.session_id));
	if (!session) {
		DBG("Relay session %" PRIu64 " not found",
				be64toh(request.session_id));
		response.status = htobe32(VIEWER_NEW_STREAMS_ERR);
		goto send_reply;
	}

	if (cmd->session_id == session->id) {
		/* We confirmed the viewer is asking for the same session. */
		send_streams = 1;
		response.status = htobe32(VIEWER_NEW_STREAMS_OK);
	} else {
		send_streams = 0;
		response.status = htobe32(VIEWER_NEW_STREAMS_ERR);
		goto send_reply;
	}

	if (!send_streams) {
		goto send_reply;
	}

	ret = make_viewer_streams(session, VIEWER_SEEK_LAST, NULL, &nb_unsent,
			&nb_created);
	if (ret < 0) {
		goto end_unlock;
	}
	/* Only send back the newly created streams with the unsent ones. */
	nb_streams = nb_created + nb_unsent;
	response.streams_count = htobe32(nb_streams);

send_reply:
	health_code_update();
	ret = send_response(cmd->sock, &response, sizeof(response));
	if (ret < 0) {
		goto end_unlock;
	}
	health_code_update();

	/*
	 * Unknown or empty session, just return gracefully, the viewer knows what
	 * is happening.
	 */
	if (!send_streams || !nb_streams) {
		ret = 0;
		goto end_unlock;
	}

	/*
	 * Send stream and *DON'T* ignore the sent flag so every viewer streams
	 * that were not sent from that point will be sent to the viewer.
	 */
	ret = send_viewer_streams(cmd->sock, session, 0);
	if (ret < 0) {
		goto end_unlock;
	}

end_unlock:
	rcu_read_unlock();
error:
	return ret;
}

/*
 * Send the viewer the list of current sessions.
 */
static
int viewer_attach_session(struct relay_command *cmd,
		struct lttng_ht *sessions_ht)
{
	int send_streams = 0;
	ssize_t ret;
	uint32_t nb_streams = 0;
	enum lttng_viewer_seek seek_type;
	struct lttng_viewer_attach_session_request request;
	struct lttng_viewer_attach_session_response response;
	struct relay_session *session;

	assert(cmd);
	assert(sessions_ht);

	health_code_update();

	/* Receive the request from the connected client. */
	ret = recv_request(cmd->sock, &request, sizeof(request));
	if (ret < 0) {
		goto error;
	}

	health_code_update();

	rcu_read_lock();
	session = session_find_by_id(sessions_ht, be64toh(request.session_id));
	if (!session) {
		DBG("Relay session %" PRIu64 " not found",
				be64toh(request.session_id));
		response.status = htobe32(VIEWER_ATTACH_UNK);
		goto send_reply;
	}
	session_viewer_attach(session);
	DBG("Attach session ID %" PRIu64 " received", be64toh(request.session_id));

	if (uatomic_read(&session->viewer_refcount) > 1) {
		DBG("Already a viewer attached");
		response.status = htobe32(VIEWER_ATTACH_ALREADY);
		session_viewer_detach(session);
		goto send_reply;
	} else if (session->live_timer == 0) {
		DBG("Not live session");
		response.status = htobe32(VIEWER_ATTACH_NOT_LIVE);
		goto send_reply;
	} else {
		send_streams = 1;
		response.status = htobe32(VIEWER_ATTACH_OK);
		cmd->session_id = session->id;
		cmd->session = session;
	}

	switch (be32toh(request.seek)) {
	case VIEWER_SEEK_BEGINNING:
	case VIEWER_SEEK_LAST:
		seek_type = be32toh(request.seek);
		break;
	default:
		ERR("Wrong seek parameter");
		response.status = htobe32(VIEWER_ATTACH_SEEK_ERR);
		send_streams = 0;
		goto send_reply;
	}

	if (!send_streams) {
		goto send_reply;
	}

	ret = make_viewer_streams(session, seek_type, &nb_streams, NULL, NULL);
	if (ret < 0) {
		goto end_unlock;
	}
	response.streams_count = htobe32(nb_streams);

send_reply:
	health_code_update();
	ret = send_response(cmd->sock, &response, sizeof(response));
	if (ret < 0) {
		goto end_unlock;
	}
	health_code_update();

	/*
	 * Unknown or empty session, just return gracefully, the viewer knows what
	 * is happening.
	 */
	if (!send_streams || !nb_streams) {
		ret = 0;
		goto end_unlock;
	}

	/* Send stream and ignore the sent flag. */
	ret = send_viewer_streams(cmd->sock, session, 1);
	if (ret < 0) {
		goto end_unlock;
	}

end_unlock:
	rcu_read_unlock();
error:
	return ret;
}

/*
 * Send the next index for a stream.
 *
 * Return 0 on success or else a negative value.
 */
static
int viewer_get_next_index(struct relay_command *cmd,
		struct lttng_ht *sessions_ht)
{
	int ret;
	struct lttng_viewer_get_next_index request_index;
	struct lttng_viewer_index viewer_index;
	struct ctf_packet_index packet_index;
	struct relay_viewer_stream *vstream;
	struct relay_stream *rstream;
	struct ctf_trace *ctf_trace;
	struct relay_session *session;

	assert(cmd);
	assert(sessions_ht);

	DBG("Viewer get next index");

	health_code_update();

	ret = recv_request(cmd->sock, &request_index, sizeof(request_index));
	if (ret < 0) {
		goto end;
	}
	health_code_update();

	rcu_read_lock();
	session = session_find_by_id(sessions_ht, cmd->session_id);
	if (!session) {
		ret = -1;
		goto end_unlock;
	}

	vstream = viewer_stream_find_by_id(be64toh(request_index.stream_id));
	if (!vstream) {
		ret = -1;
		goto end_unlock;
	}

	ctf_trace = ctf_trace_find_by_path(session->ctf_traces_ht, vstream->path_name);
	assert(ctf_trace);

	memset(&viewer_index, 0, sizeof(viewer_index));

	/*
	 * The viewer should not ask for index on metadata stream.
	 */
	if (vstream->metadata_flag) {
		viewer_index.status = htobe32(VIEWER_INDEX_HUP);
		goto send_reply;
	}

	/* First time, we open the index file */
	if (vstream->index_read_fd < 0) {
		ret = index_open(vstream->path_name, vstream->channel_name,
				vstream->tracefile_count, vstream->tracefile_count_current);
		if (ret == -ENOENT) {
			/*
			 * The index is created only when the first data packet arrives, it
			 * might not be ready at the beginning of the session
			 */
			viewer_index.status = htobe32(VIEWER_INDEX_RETRY);
			goto send_reply;
		} else if (ret < 0) {
			viewer_index.status = htobe32(VIEWER_INDEX_ERR);
			goto send_reply;
		}
		vstream->index_read_fd = ret;
	}

	rstream = stream_find_by_id(relay_streams_ht, vstream->stream_handle);
	assert(rstream);

	if (!rstream->close_flag) {
		if (vstream->abort_flag) {
			/* Rotate on abort (overwrite). */
			DBG("Viewer rotate because of overwrite");
			ret = viewer_stream_rotate(vstream, rstream);
			if (ret < 0) {
				goto end_unlock;
			} else if (ret == 1) {
				viewer_index.status = htobe32(VIEWER_INDEX_HUP);
				viewer_stream_delete(vstream);
				viewer_stream_destroy(ctf_trace, vstream);
				goto send_reply;
			}
			/* ret == 0 means successful so we continue. */
		}

		pthread_mutex_lock(&rstream->viewer_stream_rotation_lock);
		if (rstream->tracefile_count_current == vstream->tracefile_count_current) {
			if (rstream->beacon_ts_end != -1ULL &&
				vstream->last_sent_index == rstream->total_index_received) {
				viewer_index.status = htobe32(VIEWER_INDEX_INACTIVE);
				viewer_index.timestamp_end = htobe64(rstream->beacon_ts_end);
				pthread_mutex_unlock(&rstream->viewer_stream_rotation_lock);
				goto send_reply;
			} else if (rstream->total_index_received <= vstream->last_sent_index
					&& !vstream->close_write_flag) {
				/*
				 * Reader and writer are working in the same tracefile, so we care
				 * about the number of index received and sent. Otherwise, we read
				 * up to EOF.
				 */
				pthread_mutex_unlock(&rstream->viewer_stream_rotation_lock);
				/* No new index to send, retry later. */
				viewer_index.status = htobe32(VIEWER_INDEX_RETRY);
				goto send_reply;
			}
		}
		pthread_mutex_unlock(&rstream->viewer_stream_rotation_lock);
	} else if (rstream->close_flag && vstream->close_write_flag &&
			vstream->total_index_received == vstream->last_sent_index) {
		/* Last index sent and current tracefile closed in write */
		viewer_index.status = htobe32(VIEWER_INDEX_HUP);
		viewer_stream_delete(vstream);
		viewer_stream_destroy(ctf_trace, vstream);
		goto send_reply;
	} else {
		vstream->close_write_flag = 1;
	}

	if (!ctf_trace->metadata_received ||
			ctf_trace->metadata_received > ctf_trace->metadata_sent) {
		viewer_index.flags |= LTTNG_VIEWER_FLAG_NEW_METADATA;
	}

	ret = check_new_streams(vstream->session_id, sessions_ht);
	if (ret < 0) {
		goto end_unlock;
	} else if (ret == 1) {
		viewer_index.flags |= LTTNG_VIEWER_FLAG_NEW_STREAM;
	}

	pthread_mutex_lock(&vstream->overwrite_lock);
	if (vstream->abort_flag) {
		/*
		 * The file is being overwritten by the writer, we cannot * use it.
		 */
		viewer_index.status = htobe32(VIEWER_INDEX_RETRY);
		pthread_mutex_unlock(&vstream->overwrite_lock);
		ret = viewer_stream_rotate(vstream, rstream);
		if (ret < 0) {
			goto end_unlock;
		} else if (ret == 1) {
			viewer_index.status = htobe32(VIEWER_INDEX_HUP);
			viewer_stream_delete(vstream);
			viewer_stream_destroy(ctf_trace, vstream);
			goto send_reply;
		}
		goto send_reply;
	}

	ret = lttng_read(vstream->index_read_fd, &packet_index,
			sizeof(packet_index));
	pthread_mutex_unlock(&vstream->overwrite_lock);
	if (ret < sizeof(packet_index)) {
		/*
		 * The tracefile is closed in write, so we read up to EOF.
		 */
		if (vstream->close_write_flag == 1) {
			viewer_index.status = htobe32(VIEWER_INDEX_RETRY);
			/* Rotate on normal EOF */
			ret = viewer_stream_rotate(vstream, rstream);
			if (ret < 0) {
				goto end_unlock;
			} else if (ret == 1) {
				viewer_index.status = htobe32(VIEWER_INDEX_HUP);
				viewer_stream_delete(vstream);
				viewer_stream_destroy(ctf_trace, vstream);
				goto send_reply;
			}
		} else {
			PERROR("Relay reading index file %d", vstream->index_read_fd);
			viewer_index.status = htobe32(VIEWER_INDEX_ERR);
		}
		goto send_reply;
	} else {
		viewer_index.status = htobe32(VIEWER_INDEX_OK);
		vstream->last_sent_index++;
	}

	/*
	 * Indexes are stored in big endian, no need to switch before sending.
	 */
	viewer_index.offset = packet_index.offset;
	viewer_index.packet_size = packet_index.packet_size;
	viewer_index.content_size = packet_index.content_size;
	viewer_index.timestamp_begin = packet_index.timestamp_begin;
	viewer_index.timestamp_end = packet_index.timestamp_end;
	viewer_index.events_discarded = packet_index.events_discarded;
	viewer_index.stream_id = packet_index.stream_id;

send_reply:
	viewer_index.flags = htobe32(viewer_index.flags);
	health_code_update();

	ret = send_response(cmd->sock, &viewer_index, sizeof(viewer_index));
	if (ret < 0) {
		goto end_unlock;
	}
	health_code_update();

	DBG("Index %" PRIu64 " for stream %" PRIu64 " sent",
			vstream->last_sent_index, vstream->stream_handle);

end_unlock:
	rcu_read_unlock();

end:
	return ret;
}

/*
 * Send the next index for a stream
 *
 * Return 0 on success or else a negative value.
 */
static
int viewer_get_packet(struct relay_command *cmd,
		struct lttng_ht *sessions_ht)
{
	int ret, send_data = 0;
	char *data = NULL;
	uint32_t len = 0;
	ssize_t read_len;
	struct lttng_viewer_get_packet get_packet_info;
	struct lttng_viewer_trace_packet reply;
	struct relay_viewer_stream *stream;
	struct ctf_trace *ctf_trace;

	assert(cmd);

	DBG2("Relay get data packet");

	health_code_update();

	ret = recv_request(cmd->sock, &get_packet_info, sizeof(get_packet_info));
	if (ret < 0) {
		goto end;
	}
	health_code_update();

	/* From this point on, the error label can be reached. */
	memset(&reply, 0, sizeof(reply));

	rcu_read_lock();
	stream = viewer_stream_find_by_id(be64toh(get_packet_info.stream_id));
	if (!stream) {
		goto error;
	}

	ctf_trace = ctf_trace_find_by_path(cmd->session->ctf_traces_ht,
			stream->path_name);
	assert(ctf_trace);

	/*
	 * First time we read this stream, we need open the tracefile, we should
	 * only arrive here if an index has already been sent to the viewer, so the
	 * tracefile must exist, if it does not it is a fatal error.
	 */
	if (stream->read_fd < 0) {
		char fullpath[PATH_MAX];

		if (stream->tracefile_count > 0) {
			ret = snprintf(fullpath, PATH_MAX, "%s/%s_%" PRIu64, stream->path_name,
					stream->channel_name,
					stream->tracefile_count_current);
		} else {
			ret = snprintf(fullpath, PATH_MAX, "%s/%s", stream->path_name,
					stream->channel_name);
		}
		if (ret < 0) {
			goto error;
		}
		ret = open(fullpath, O_RDONLY);
		if (ret < 0) {
			PERROR("Relay opening trace file");
			goto error;
		}
		stream->read_fd = ret;
	}

	if (!ctf_trace->metadata_received ||
			ctf_trace->metadata_received > ctf_trace->metadata_sent) {
		reply.status = htobe32(VIEWER_GET_PACKET_ERR);
		reply.flags |= LTTNG_VIEWER_FLAG_NEW_METADATA;
		goto send_reply;
	}

	ret = check_new_streams(stream->session_id, sessions_ht);
	if (ret < 0) {
		goto end_unlock;
	} else if (ret == 1) {
		reply.status = htobe32(VIEWER_GET_PACKET_ERR);
		reply.flags |= LTTNG_VIEWER_FLAG_NEW_STREAM;
		goto send_reply;
	}

	len = be32toh(get_packet_info.len);
	data = zmalloc(len);
	if (!data) {
		PERROR("relay data zmalloc");
		goto error;
	}

	ret = lseek(stream->read_fd, be64toh(get_packet_info.offset), SEEK_SET);
	if (ret < 0) {
		/*
		 * If the read fd was closed by the streaming side, the
		 * abort_flag will be set to 1, otherwise it is an error.
		 */
		if (stream->abort_flag == 0) {
			PERROR("lseek");
			goto error;
		}
		reply.status = htobe32(VIEWER_GET_PACKET_EOF);
		goto send_reply;
	}
	read_len = lttng_read(stream->read_fd, data, len);
	if (read_len < len) {
		/*
		 * If the read fd was closed by the streaming side, the
		 * abort_flag will be set to 1, otherwise it is an error.
		 */
		if (stream->abort_flag == 0) {
			PERROR("Relay reading trace file, fd: %d, offset: %" PRIu64,
					stream->read_fd,
					be64toh(get_packet_info.offset));
			goto error;
		} else {
			reply.status = htobe32(VIEWER_GET_PACKET_EOF);
			goto send_reply;
		}
	}
	reply.status = htobe32(VIEWER_GET_PACKET_OK);
	reply.len = htobe32(len);
	send_data = 1;
	goto send_reply;

error:
	reply.status = htobe32(VIEWER_GET_PACKET_ERR);

send_reply:
	reply.flags = htobe32(reply.flags);

	health_code_update();

	ret = send_response(cmd->sock, &reply, sizeof(reply));
	if (ret < 0) {
		goto end_unlock;
	}
	health_code_update();

	if (send_data) {
		health_code_update();
		ret = send_response(cmd->sock, data, len);
		if (ret < 0) {
			goto end_unlock;
		}
		health_code_update();
	}

	DBG("Sent %u bytes for stream %" PRIu64, len,
			be64toh(get_packet_info.stream_id));

end_unlock:
	free(data);
	rcu_read_unlock();

end:
	return ret;
}

/*
 * Send the session's metadata
 *
 * Return 0 on success else a negative value.
 */
static
int viewer_get_metadata(struct relay_command *cmd)
{
	int ret = 0;
	ssize_t read_len;
	uint64_t len = 0;
	char *data = NULL;
	struct lttng_viewer_get_metadata request;
	struct lttng_viewer_metadata_packet reply;
	struct relay_viewer_stream *stream;
	struct ctf_trace *ctf_trace;

	assert(cmd);

	DBG("Relay get metadata");

	health_code_update();

	ret = recv_request(cmd->sock, &request, sizeof(request));
	if (ret < 0) {
		goto end;
	}
	health_code_update();

	rcu_read_lock();
	stream = viewer_stream_find_by_id(be64toh(request.stream_id));
	if (!stream || !stream->metadata_flag) {
		ERR("Invalid metadata stream");
		goto error;
	}

	ctf_trace = ctf_trace_find_by_path(cmd->session->ctf_traces_ht,
			stream->path_name);
	assert(ctf_trace);
	assert(ctf_trace->metadata_sent <= ctf_trace->metadata_received);

	len = ctf_trace->metadata_received - ctf_trace->metadata_sent;
	if (len == 0) {
		reply.status = htobe32(VIEWER_NO_NEW_METADATA);
		goto send_reply;
	}

	/* first time, we open the metadata file */
	if (stream->read_fd < 0) {
		char fullpath[PATH_MAX];

		ret = snprintf(fullpath, PATH_MAX, "%s/%s", stream->path_name,
				stream->channel_name);
		if (ret < 0) {
			goto error;
		}
		ret = open(fullpath, O_RDONLY);
		if (ret < 0) {
			PERROR("Relay opening metadata file");
			goto error;
		}
		stream->read_fd = ret;
	}

	reply.len = htobe64(len);
	data = zmalloc(len);
	if (!data) {
		PERROR("viewer metadata zmalloc");
		goto error;
	}

	read_len = lttng_read(stream->read_fd, data, len);
	if (read_len < len) {
		PERROR("Relay reading metadata file");
		goto error;
	}
	ctf_trace->metadata_sent += read_len;
	reply.status = htobe32(VIEWER_METADATA_OK);
	goto send_reply;

error:
	reply.status = htobe32(VIEWER_METADATA_ERR);

send_reply:
	health_code_update();
	ret = send_response(cmd->sock, &reply, sizeof(reply));
	if (ret < 0) {
		goto end_unlock;
	}
	health_code_update();

	if (len > 0) {
		ret = send_response(cmd->sock, data, len);
		if (ret < 0) {
			goto end_unlock;
		}
	}

	DBG("Sent %" PRIu64 " bytes of metadata for stream %" PRIu64, len,
			be64toh(request.stream_id));

	DBG("Metadata sent");

end_unlock:
	free(data);
	rcu_read_unlock();
end:
	return ret;
}

/*
 * live_relay_unknown_command: send -1 if received unknown command
 */
static
void live_relay_unknown_command(struct relay_command *cmd)
{
	struct lttcomm_relayd_generic_reply reply;

	reply.ret_code = htobe32(LTTNG_ERR_UNK);
	(void) send_response(cmd->sock, &reply, sizeof(reply));
}

/*
 * Process the commands received on the control socket
 */
static
int process_control(struct lttng_viewer_cmd *recv_hdr,
		struct relay_command *cmd, struct lttng_ht *sessions_ht)
{
	int ret = 0;
	uint32_t msg_value;

	assert(recv_hdr);
	assert(cmd);
	assert(sessions_ht);

	msg_value = be32toh(recv_hdr->cmd);

	/*
	 * Make sure we've done the version check before any command other then a
	 * new client connection.
	 */
	if (msg_value != VIEWER_CONNECT && !cmd->version_check_done) {
		ERR("Viewer cmd value %" PRIu32 " before version check", msg_value);
		ret = -1;
		goto end;
	}

	switch (msg_value) {
	case VIEWER_CONNECT:
		ret = viewer_connect(cmd);
		break;
	case VIEWER_LIST_SESSIONS:
		ret = viewer_list_sessions(cmd, sessions_ht);
		break;
	case VIEWER_ATTACH_SESSION:
		ret = viewer_attach_session(cmd, sessions_ht);
		break;
	case VIEWER_GET_NEXT_INDEX:
		ret = viewer_get_next_index(cmd, sessions_ht);
		break;
	case VIEWER_GET_PACKET:
		ret = viewer_get_packet(cmd, sessions_ht);
		break;
	case VIEWER_GET_METADATA:
		ret = viewer_get_metadata(cmd);
		break;
	case VIEWER_GET_NEW_STREAMS:
		ret = viewer_get_new_streams(cmd, sessions_ht);
		break;
	default:
		ERR("Received unknown viewer command (%u)", be32toh(recv_hdr->cmd));
		live_relay_unknown_command(cmd);
		ret = -1;
		goto end;
	}

end:
	return ret;
}

static
void cleanup_poll_connection(struct lttng_poll_event *events, int pollfd)
{
	int ret;

	assert(events);

	lttng_poll_del(events, pollfd);

	ret = close(pollfd);
	if (ret < 0) {
		ERR("Closing pollfd %d", pollfd);
	}
}

/*
 * Create and add connection to the given hash table.
 *
 * Return poll add value or else -1 on error.
 */
static
int add_connection(int fd, struct lttng_poll_event *events,
		struct lttng_ht *relay_connections_ht)
{
	int ret;
	struct relay_command *relay_connection;

	assert(events);
	assert(relay_connections_ht);

	relay_connection = zmalloc(sizeof(struct relay_command));
	if (relay_connection == NULL) {
		PERROR("Relay command zmalloc");
		goto error;
	}

	ret = lttng_read(fd, relay_connection, sizeof(*relay_connection));
	if (ret < sizeof(*relay_connection)) {
		PERROR("read relay cmd pipe");
		goto error_read;
	}

	lttng_ht_node_init_ulong(&relay_connection->sock_n,
			(unsigned long) relay_connection->sock->fd);
	rcu_read_lock();
	lttng_ht_add_unique_ulong(relay_connections_ht,
			&relay_connection->sock_n);
	rcu_read_unlock();

	return lttng_poll_add(events, relay_connection->sock->fd,
			LPOLLIN | LPOLLRDHUP);

error_read:
	free(relay_connection);
error:
	return -1;
}

static
void deferred_free_connection(struct rcu_head *head)
{
	struct relay_command *relay_connection =
		caa_container_of(head, struct relay_command, rcu_node);

	lttcomm_destroy_sock(relay_connection->sock);
	free(relay_connection);
}

/*
 * Delete all streams for a specific session ID.
 */
static void destroy_viewer_streams_by_session(struct relay_session *session)
{
	struct relay_viewer_stream *stream;
	struct lttng_ht_iter iter;

	assert(session);

	rcu_read_lock();
	cds_lfht_for_each_entry(viewer_streams_ht->ht, &iter.iter, stream,
			stream_n.node) {
		struct ctf_trace *ctf_trace;

		health_code_update();
		if (stream->session_id != session->id) {
			continue;
		}

		ctf_trace = ctf_trace_find_by_path(session->ctf_traces_ht,
				stream->path_name);
		assert(ctf_trace);

		viewer_stream_delete(stream);

		if (stream->metadata_flag) {
			ctf_trace->metadata_sent = 0;
			ctf_trace->viewer_metadata_stream = NULL;
		}

		viewer_stream_destroy(ctf_trace, stream);
	}
	rcu_read_unlock();
}

static void try_destroy_streams(struct relay_session *session)
{
	struct ctf_trace *ctf_trace;
	struct lttng_ht_iter iter;

	assert(session);

	cds_lfht_for_each_entry(session->ctf_traces_ht->ht, &iter.iter, ctf_trace,
			node.node) {
		/* Attempt to destroy the ctf trace of that session. */
		ctf_trace_try_destroy(session, ctf_trace);
	}
}

/*
 * Delete and free a connection.
 *
 * RCU read side lock MUST be acquired.
 */
static
void del_connection(struct lttng_ht *relay_connections_ht,
		struct lttng_ht_iter *iter, struct relay_command *relay_connection,
		struct lttng_ht *sessions_ht)
{
	int ret;
	struct relay_session *session;

	assert(relay_connections_ht);
	assert(iter);
	assert(relay_connection);
	assert(sessions_ht);

	DBG("Cleaning connection of session ID %" PRIu64,
			relay_connection->session_id);

	rcu_read_lock();
	ret = lttng_ht_del(relay_connections_ht, iter);
	assert(!ret);

	session = session_find_by_id(sessions_ht, relay_connection->session_id);
	if (session) {
		/*
		 * Very important that this is done before destroying the session so we
		 * can put back every viewer stream reference from the ctf_trace.
		 */
		destroy_viewer_streams_by_session(session);
		try_destroy_streams(session);
		session_viewer_try_destroy(sessions_ht, session);
	}
	rcu_read_unlock();

	call_rcu(&relay_connection->rcu_node, deferred_free_connection);
}

/*
 * This thread does the actual work
 */
static
void *thread_worker(void *data)
{
	int ret, err = -1;
	uint32_t nb_fd;
	struct relay_command *relay_connection;
	struct lttng_poll_event events;
	struct lttng_ht *relay_connections_ht;
	struct lttng_ht_node_ulong *node;
	struct lttng_ht_iter iter;
	struct lttng_viewer_cmd recv_hdr;
	struct relay_local_data *relay_ctx = (struct relay_local_data *) data;
	struct lttng_ht *sessions_ht = relay_ctx->sessions_ht;

	DBG("[thread] Live viewer relay worker started");

	rcu_register_thread();

	health_register(health_relayd, HEALTH_RELAYD_TYPE_LIVE_WORKER);

	if (testpoint(relayd_thread_live_worker)) {
		goto error_testpoint;
	}

	/* table of connections indexed on socket */
	relay_connections_ht = lttng_ht_new(0, LTTNG_HT_TYPE_ULONG);
	if (!relay_connections_ht) {
		goto relay_connections_ht_error;
	}

	ret = create_thread_poll_set(&events, 2);
	if (ret < 0) {
		goto error_poll_create;
	}

	ret = lttng_poll_add(&events, live_relay_cmd_pipe[0], LPOLLIN | LPOLLRDHUP);
	if (ret < 0) {
		goto error;
	}

restart:
	while (1) {
		int i;

		health_code_update();

		/* Infinite blocking call, waiting for transmission */
		DBG3("Relayd live viewer worker thread polling...");
		health_poll_entry();
		ret = lttng_poll_wait(&events, -1);
		health_poll_exit();
		if (ret < 0) {
			/*
			 * Restart interrupted system call.
			 */
			if (errno == EINTR) {
				goto restart;
			}
			goto error;
		}

		nb_fd = ret;

		/*
		 * Process control. The control connection is prioritised so we don't
		 * starve it with high throughput tracing data on the data
		 * connection.
		 */
		for (i = 0; i < nb_fd; i++) {
			/* Fetch once the poll data */
			uint32_t revents = LTTNG_POLL_GETEV(&events, i);
			int pollfd = LTTNG_POLL_GETFD(&events, i);

			health_code_update();

			/* Thread quit pipe has been closed. Killing thread. */
			ret = check_live_conn_pipe(pollfd, revents);
			if (ret) {
				err = 0;
				goto exit;
			}

			/* Inspect the relay cmd pipe for new connection */
			if (pollfd == live_relay_cmd_pipe[0]) {
				if (revents & (LPOLLERR | LPOLLHUP | LPOLLRDHUP)) {
					ERR("Relay live pipe error");
					goto error;
				} else if (revents & LPOLLIN) {
					DBG("Relay live viewer command received");
					ret = add_connection(live_relay_cmd_pipe[0],
							&events, relay_connections_ht);
					if (ret < 0) {
						goto error;
					}
				}
			} else if (revents) {
				rcu_read_lock();
				lttng_ht_lookup(relay_connections_ht,
						(void *)((unsigned long) pollfd), &iter);
				node = lttng_ht_iter_get_node_ulong(&iter);
				if (node == NULL) {
					DBG2("Relay viewer sock %d not found", pollfd);
					rcu_read_unlock();
					goto error;
				}
				relay_connection = caa_container_of(node, struct relay_command,
						sock_n);

				if (revents & (LPOLLERR)) {
					cleanup_poll_connection(&events, pollfd);
					del_connection(relay_connections_ht, &iter,
							relay_connection, relay_ctx->sessions_ht);
				} else if (revents & (LPOLLHUP | LPOLLRDHUP)) {
					DBG("Viewer socket %d hung up", pollfd);
					cleanup_poll_connection(&events, pollfd);
					del_connection(relay_connections_ht, &iter,
							relay_connection, relay_ctx->sessions_ht);
				} else if (revents & LPOLLIN) {
					ret = relay_connection->sock->ops->recvmsg(
							relay_connection->sock, &recv_hdr,
							sizeof(struct lttng_viewer_cmd),
							0);
					/* connection closed */
					if (ret <= 0) {
						cleanup_poll_connection(&events, pollfd);
						del_connection(relay_connections_ht, &iter,
								relay_connection, relay_ctx->sessions_ht);
						DBG("Viewer control connection closed with %d",
								pollfd);
					} else {
						if (relay_connection->session) {
							DBG2("Relay viewer worker receiving data for "
									"session: %" PRIu64,
									relay_connection->session->id);
						}
						ret = process_control(&recv_hdr, relay_connection,
								sessions_ht);
						if (ret < 0) {
							/* Clear the session on error. */
							cleanup_poll_connection(&events, pollfd);
							del_connection(relay_connections_ht, &iter,
									relay_connection, relay_ctx->sessions_ht);
							DBG("Viewer connection closed with %d", pollfd);
						}
					}
				}
				rcu_read_unlock();
			}
		}
	}

exit:
error:
	lttng_poll_clean(&events);

	/* empty the hash table and free the memory */
	rcu_read_lock();
	cds_lfht_for_each_entry(relay_connections_ht->ht, &iter.iter, node, node) {
		health_code_update();

		node = lttng_ht_iter_get_node_ulong(&iter);
		if (!node) {
			continue;
		}

		relay_connection = caa_container_of(node, struct relay_command,
				sock_n);
		del_connection(relay_connections_ht, &iter, relay_connection,
				relay_ctx->sessions_ht);
	}
	rcu_read_unlock();
error_poll_create:
	lttng_ht_destroy(relay_connections_ht);
relay_connections_ht_error:
	/* Close relay cmd pipes */
	utils_close_pipe(live_relay_cmd_pipe);
	if (err) {
		DBG("Viewer worker thread exited with error");
	}
	DBG("Viewer worker thread cleanup complete");
error_testpoint:
	if (err) {
		health_error();
		ERR("Health error occurred in %s", __func__);
	}
	health_unregister(health_relayd);
	stop_threads();
	rcu_unregister_thread();
	return NULL;
}

/*
 * Create the relay command pipe to wake thread_manage_apps.
 * Closed in cleanup().
 */
static int create_relay_cmd_pipe(void)
{
	int ret;

	ret = utils_create_pipe_cloexec(live_relay_cmd_pipe);

	return ret;
}

void live_stop_threads(void)
{
	int ret;
	void *status;

	stop_threads();

	ret = pthread_join(live_listener_thread, &status);
	if (ret != 0) {
		PERROR("pthread_join live listener");
		goto error;	/* join error, exit without cleanup */
	}

	ret = pthread_join(live_worker_thread, &status);
	if (ret != 0) {
		PERROR("pthread_join live worker");
		goto error;	/* join error, exit without cleanup */
	}

	ret = pthread_join(live_dispatcher_thread, &status);
	if (ret != 0) {
		PERROR("pthread_join live dispatcher");
		goto error;	/* join error, exit without cleanup */
	}

	cleanup();

error:
	return;
}

/*
 * main
 */
int live_start_threads(struct lttng_uri *uri,
		struct relay_local_data *relay_ctx)
{
	int ret = 0;
	void *status;
	int is_root;

	assert(uri);
	live_uri = uri;

	/* Check if daemon is UID = 0 */
	is_root = !getuid();

	if (!is_root) {
		if (live_uri->port < 1024) {
			ERR("Need to be root to use ports < 1024");
			ret = -1;
			goto exit;
		}
	}

	/* Setup the thread apps communication pipe. */
	if ((ret = create_relay_cmd_pipe()) < 0) {
		goto exit;
	}

	/* Init relay command queue. */
	cds_wfq_init(&viewer_cmd_queue.queue);

	/* Set up max poll set size */
	lttng_poll_set_max_size();

	/* Setup the dispatcher thread */
	ret = pthread_create(&live_dispatcher_thread, NULL,
			thread_dispatcher, (void *) NULL);
	if (ret != 0) {
		PERROR("pthread_create viewer dispatcher");
		goto exit_dispatcher;
	}

	/* Setup the worker thread */
	ret = pthread_create(&live_worker_thread, NULL,
			thread_worker, relay_ctx);
	if (ret != 0) {
		PERROR("pthread_create viewer worker");
		goto exit_worker;
	}

	/* Setup the listener thread */
	ret = pthread_create(&live_listener_thread, NULL,
			thread_listener, (void *) NULL);
	if (ret != 0) {
		PERROR("pthread_create viewer listener");
		goto exit_listener;
	}

	ret = 0;
	goto end;

exit_listener:
	ret = pthread_join(live_listener_thread, &status);
	if (ret != 0) {
		PERROR("pthread_join live listener");
		goto error;	/* join error, exit without cleanup */
	}

exit_worker:
	ret = pthread_join(live_worker_thread, &status);
	if (ret != 0) {
		PERROR("pthread_join live worker");
		goto error;	/* join error, exit without cleanup */
	}

exit_dispatcher:
	ret = pthread_join(live_dispatcher_thread, &status);
	if (ret != 0) {
		PERROR("pthread_join live dispatcher");
		goto error;	/* join error, exit without cleanup */
	}

exit:
	cleanup();

end:
error:
	return ret;
}
