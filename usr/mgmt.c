/*
 * SCSI target management functions
 *
 * Copyright (C) 2005 FUJITA Tomonori <tomof@acm.org>
 * Copyright (C) 2005 Mike Christie <michaelc@cs.wisc.edu>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301 USA
 */
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>

#include "list.h"
#include "tgtd.h"
#include "log.h"
#include "tgtadm.h"
#include "driver.h"

#define BUFSIZE 4096

static void set_show_results(struct tgtadm_res *res, int *err)
{
	if (err < 0)
		res->err = *err;
	else {
		res->err = 0;
		res->len = *err + sizeof(*res);
		*err = 0;
	}
}

static int target_mgmt(int lld_no, struct tgtadm_req *req, char *params,
		       struct tgtadm_res *res, int *rlen)
{
	int err = -EINVAL;

	switch (req->op) {
	case OP_NEW:
		err = tgt_target_create(lld_no, req->tid);
		if (!err && tgt_drivers[lld_no]->target_create)
			tgt_drivers[lld_no]->target_create(req->tid, params);
		break;
	case OP_DELETE:
		err = tgt_target_destroy(req->tid);
		if (!err && tgt_drivers[lld_no]->target_destroy)
			tgt_drivers[lld_no]->target_destroy(req->tid);
		break;
	case OP_BIND:
		err = tgt_target_bind(req->tid, req->host_no, lld_no);
		break;
	case OP_UPDATE:
		err = -EINVAL;
		if (!strcmp(params, "state"))
			err = tgt_set_target_state(req->tid,
						   params + strlen(params) + 1);
		else if (!strcmp(params, "iotype"))
			err = tgt_set_target_iotype(req->tid,
						    params + strlen(params) + 1);
		else if (tgt_drivers[lld_no]->target_update)
			err = tgt_drivers[lld_no]->target_update(req->tid, params);
		break;
	case OP_SHOW:
		if (req->tid < 0)
			err = tgt_target_show_all((char *)res->data,
						  *rlen - sizeof(*res));
		else if (tgt_drivers[lld_no]->show)
			err = tgt_drivers[lld_no]->show(req->mode,
							req->tid, req->sid,
							req->cid, req->lun,
							(char *)res->data,
							*rlen - sizeof(*res));
		break;
	default:
		break;
	}

	if (req->op == OP_SHOW)
		set_show_results(res, &err);
	else {
		res->err = err;
		res->len = (char *) res->data - (char *) res;
	}
	return err;
}

static int device_mgmt(int lld_no, struct tgtadm_req *req, char *params,
		       struct tgtadm_res *res, int *rlen)
{
	int err = -EINVAL;

	switch (req->op) {
	case OP_NEW:
		err = tgt_device_create(req->tid, req->lun);
		break;
	case OP_DELETE:
		err = tgt_device_destroy(req->tid, req->lun);
		break;
	case OP_UPDATE:
		err = tgt_device_update(req->tid, req->lun, params);
		break;
	case OP_SHOW:
		err = tgt_device_show(req->tid, req->lun, (char *) res->data,
				      *rlen - sizeof(*res));
		break;
	default:
		break;
	}

	if (req->op == OP_SHOW)
		set_show_results(res, &err);
	else {
		res->err = err;
		res->len = sizeof(*res);
	}

	return err;
}

int tgt_mgmt(int lld_no, struct tgtadm_req *req, struct tgtadm_res *res,
	     int len)
{
	int err = -EINVAL;
	char *params = (char *) req->data;

	dprintf("%d %d %d %d %d %" PRIx64 " %" PRIx64 " %s %d\n",
		req->len, lld_no, req->mode, req->op,
		req->tid, req->sid, req->lun, params, getpid());

	switch (req->mode) {
	case MODE_SYSTEM:
		break;
	case MODE_TARGET:
		err = target_mgmt(lld_no, req, params, res, &len);
		break;
	case MODE_DEVICE:
		err = device_mgmt(lld_no, req, params, res, &len);
		break;
	case MODE_ACCOUNT:
		if (tgt_drivers[lld_no]->account)
			err = tgt_drivers[lld_no]->account(req->op, req->tid, req->aid,
							   params,
							   (char *)res->data,
							   len - sizeof(*res));
		if (req->op == OP_SHOW) {
			set_show_results(res, &err);
			err = 0;
		} else {
			res->err = err;
			res->len = sizeof(*res);
		}
		break;
	default:
		if (req->op == OP_SHOW && tgt_drivers[lld_no]->show) {
			err = tgt_drivers[lld_no]->show(req->mode,
							req->tid, req->sid,
							req->cid, req->lun,
							(char *)res->data,
							len - sizeof(*res));

			set_show_results(res, &err);
		}
		break;
	}

	return err;
}

static int ipc_accept(int accept_fd)
{
	struct sockaddr addr;
	socklen_t len;
	int fd;

	len = sizeof(addr);
	fd = accept(accept_fd, (struct sockaddr *) &addr, &len);
	if (fd < 0)
		eprintf("can't accept a new connection, %m\n");
	return fd;
}

static int ipc_perm(int fd)
{
	struct ucred cred;
	socklen_t len;
	int err;

	len = sizeof(cred);
	err = getsockopt(fd, SOL_SOCKET, SO_PEERCRED, (void *) &cred, &len);
	if (err) {
		eprintf("can't get sockopt, %m\n");
		return -1;
	}

	if (cred.uid || cred.gid)
		return -EPERM;

	return 0;
}

static void ipc_send_res(int fd, struct tgtadm_res *res)
{
	struct iovec iov;
	struct msghdr msg;
	int err;

	iov.iov_base = res;
	iov.iov_len = res->len;
	memset(&msg, 0, sizeof(msg));
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;

	err = sendmsg(fd, &msg, MSG_DONTWAIT);
	if (err != res->len)
		eprintf("can't write, %m\n");
}

static void mgmt_event_handler(int accept_fd, int events, void *data)
{
	int fd, err;
	char sbuf[BUFSIZE], rbuf[BUFSIZE];
	struct iovec iov;
	struct msghdr msg;
	struct tgtadm_req *req;
	struct tgtadm_res *res;
	int lld_no, len;

	req = (struct tgtadm_req *) sbuf;
	memset(sbuf, 0, sizeof(sbuf));

	fd = ipc_accept(accept_fd);
	if (fd < 0)
		return;

	err = ipc_perm(fd);
	if (err < 0)
		goto out;

	len = (char *) req->data - (char *) req;
	iov.iov_base = req;
	iov.iov_len = len;
	memset(&msg, 0, sizeof(msg));
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;

retry:
	err = recvmsg(fd, &msg, MSG_PEEK | MSG_DONTWAIT);
	if (err != len) {
		/*
		 * workaround. We need to put this request to
		 * scheduler and wait for timeout or data.
		 */
		if (errno == EAGAIN)
			goto retry;

		eprintf("can't read, %m\n");
		goto out;
	}

	if (req->len > sizeof(sbuf) - len) {
		eprintf("too long data %d\n", req->len);
		goto out;
	}

	iov.iov_base = req;
	iov.iov_len = req->len;
	memset(&msg, 0, sizeof(msg));
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;

	err = recvmsg(fd, &msg, MSG_DONTWAIT);
	if (err != req->len) {
		eprintf("can't read, %m\n");
		err = -EIO;
		goto out;
	}

	dprintf("%d %s %d %d %d\n", req->mode, req->lld, err, req->len, fd);
	res = (struct tgtadm_res *) rbuf;
	memset(rbuf, 0, sizeof(rbuf));

	lld_no = get_driver_index(req->lld);
	if (lld_no < 0) {
		eprintf("can't find the driver\n");
		res->err = ENOENT;
		res->len = (char *) res->data - (char *) res;
		goto send;
	}

	err = tgt_mgmt(lld_no, req, res, sizeof(rbuf));
	if (err)
		eprintf("%d %d %d %d\n", req->mode, lld_no, err, res->len);

send:
	ipc_send_res(fd, res);
out:
	if (fd > 0)
		close(fd);

	return;
}

int ipc_init(void)
{
	int fd, err;
	struct sockaddr_un addr;

	fd = socket(AF_LOCAL, SOCK_STREAM, 0);
	if (fd < 0) {
		eprintf("can't open a socket, %m\n");
		return -1;
	}

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_LOCAL;
	memcpy((char *) &addr.sun_path + 1, TGT_IPC_NAMESPACE,
	       strlen(TGT_IPC_NAMESPACE));

	err = bind(fd, (struct sockaddr *) &addr, sizeof(addr));
	if (err) {
		eprintf("can't bind a socket, %m\n");
		goto out;
	}

	err = listen(fd, 32);
	if (err) {
		eprintf("can't listen a socket, %m\n");
		goto out;
	}

	err = tgt_event_add(fd, EPOLLIN, mgmt_event_handler, NULL);
	if (err)
		goto out;

	return 0;
out:
	close(fd);
	return -1;
}
