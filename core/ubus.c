/*
 * (C) Copyright 2021
 * Anton Kikin, Tano Systems LLC, a.kikin@tano-systems.com
 *
 * SPDX-License-Identifier:     MIT
 */

#include <stdio.h>
#include <sys/select.h>
#include <pthread.h>
#include "util.h"
#include "pctl.h"
#include "progress_ipc.h"

#include <libubus.h>

static void ubus_notify_info(
	RECOVERY_STATUS status, int error, int level, const char *msg);

static void *ubus_thread(void __attribute__ ((__unused__)) *data);

static void ubus_subscribe_cb(
	struct ubus_context __attribute__ ((__unused__)) *ctx,
	struct ubus_object *obj);

static int ubus_method_status(
	struct ubus_context *ctx, struct ubus_object *obj,
	struct ubus_request_data *req, const char *method,
	struct blob_attr *msg);


static struct ubus_context *ubus_ctx;
static pthread_mutex_t ubus_lock;
static pthread_t ubus_thread_id = 0;
static int ubus_thread_stop = 0;
static int ubus_has_subscribers = 0;

struct progress_msg ubus_last_status;

static struct blob_buf b;


static const struct ubus_method ubus_methods[] = {
	UBUS_METHOD_NOARG("status", ubus_method_status)
};

static struct ubus_object_type ubus_object_type =
	UBUS_OBJECT_TYPE("swupdate", ubus_methods);

static struct ubus_object ubus_object = {
	.name = "swupdate",
	.type = &ubus_object_type,
	.methods = ubus_methods,
	.n_methods = ARRAY_SIZE(ubus_methods),
	.subscribe_cb = ubus_subscribe_cb,
};

static void *ubus_thread(void __attribute__ ((__unused__)) *data)
{
	fd_set fds;
	int ret;

	TRACE("Started ubus service thread");

	while (1) {
		FD_ZERO(&fds);
		FD_SET(ubus_ctx->sock.fd, &fds);

		ret = select(ubus_ctx->sock.fd + 1, &fds, NULL, NULL, NULL);

		if (ubus_thread_stop)
			break;

		if (ret > 0 && FD_ISSET(ubus_ctx->sock.fd, &fds)) {
			pthread_mutex_lock(&ubus_lock);
			ubus_handle_event(ubus_ctx);
			pthread_mutex_unlock(&ubus_lock);
		}
	}

	return NULL;
}

/* The ubus_lock mutex is locked when this function is called */
static void ubus_subscribe_cb(
	struct ubus_context __attribute__ ((__unused__)) *ctx,
	struct ubus_object *obj)
{
	ubus_has_subscribers = obj->has_subscribers;
}

int ubus_init(const char *ubus_socket)
{
	int ret;

	memset(&ubus_last_status, 0, sizeof(ubus_last_status));

	pthread_mutex_init(&ubus_lock, NULL);

	ubus_ctx = ubus_connect(ubus_socket);
	if (!ubus_ctx) {
		fprintf(stderr, "ubus_connect() failed\n");
		return -1;
	}

	ret = ubus_add_object(ubus_ctx, &ubus_object);
	if (ret) {
		fprintf(stderr, "ubus_add_object() failed: %s\n",
			ubus_strerror(ret));

		ubus_free(ubus_ctx);
		return -1;
	}

	TRACE("Connected to ubus");

	ubus_thread_id = start_thread(ubus_thread, NULL);

	ret = register_notifier(ubus_notify_info);
	if (ret) {
		ubus_stop();
		pthread_join(ubus_thread_id, NULL);
		ubus_free(ubus_ctx);
	}

	return ret;
}

void ubus_stop(void)
{
	ubus_thread_stop = 1;
	if (ubus_thread_id > 0)
		pthread_kill(ubus_thread_id, SIGINT);
}

/* Function taken from mongoose_interface.c */
#define enum_string(x)	[x] = #x
static const char *get_status_string(unsigned int status)
{
	const char *const str[] = {
		enum_string(IDLE),
		enum_string(START),
		enum_string(RUN),
		enum_string(SUCCESS),
		enum_string(FAILURE),
		enum_string(DOWNLOAD),
		enum_string(DONE),
		enum_string(SUBPROCESS),
		enum_string(PROGRESS),
	};

	if (status >= ARRAY_SIZE(str))
		return "UNKNOWN";

	return str[status];
}

/* Function taken from mongoose_interface.c */
#define enum_source_string(x)	[SOURCE_##x] = #x
static const char *get_source_string(unsigned int source)
{
	const char *const str[] = {
		enum_source_string(UNKNOWN),
		enum_source_string(WEBSERVER),
		enum_source_string(SURICATTA),
		enum_source_string(DOWNLOADER),
		enum_source_string(LOCAL)
	};

	if (source >= ARRAY_SIZE(str))
		return "UNKNOWN";

	return str[source];
}

#define enum_level_string(x)	[x##LEVEL] = #x
static const char *get_level_string(int level)
{
	const char *const str[] = {
		enum_level_string(ERROR),
		enum_level_string(WARN),
		enum_level_string(INFO),
		enum_level_string(DEBUG),
		enum_level_string(TRACE)
	};

	if (level >= ARRAY_SIZE(str))
		return "UNKNOWN";

	return str[level];
}

static void blob_fill_progress_msg(
	struct blob_buf *blob, const struct progress_msg *msg)
{
	blobmsg_add_u32    (blob, "source",      msg->source);
	blobmsg_add_string (blob, "source_str",  get_source_string(msg->source));
	blobmsg_add_u32    (blob, "status",      msg->status);
	blobmsg_add_string (blob, "status_str",  get_status_string(msg->status));
	blobmsg_add_u32    (blob, "dwl_percent", msg->dwl_percent);
	blobmsg_add_u64    (blob, "dwl_bytes",   msg->dwl_bytes);
	blobmsg_add_u32    (blob, "nsteps",      msg->nsteps);
	blobmsg_add_u32    (blob, "cur_step",    msg->cur_step);
	blobmsg_add_u32    (blob, "cur_percent", msg->cur_percent);

	if (msg->infolen > 0)
		blobmsg_add_string(blob, "info", msg->info);
}

/* The ubus_lock mutex is locked when this function is called */
static int ubus_method_status(
	struct ubus_context *ctx, struct ubus_object *obj,
	struct ubus_request_data *req, const char *method,
	struct blob_attr *msg)
{
	blob_buf_init(&b, 0);
	blob_fill_progress_msg(&b, &ubus_last_status);
	ubus_send_reply(ctx, req, b.head);
	return UBUS_STATUS_OK;
}

void ubus_notify_progress(struct progress_msg *msg)
{
	if (ubus_thread_id <= 0)
		return;

	pthread_mutex_lock(&ubus_lock);

	memcpy(&ubus_last_status, msg, sizeof(ubus_last_status));

	if (!ubus_has_subscribers) {
		/* Do not send notifications if no active subscribers */
		goto end;
	}

	blob_buf_init(&b, 0);
	blob_fill_progress_msg(&b, &ubus_last_status);

	/* -1 = do not wait for reply from ubusd */
	ubus_notify(ubus_ctx, &ubus_object, "progress", b.head, -1);

end:
	pthread_mutex_unlock(&ubus_lock);
}

void ubus_notify_info(
	RECOVERY_STATUS status, int error, int level, const char *msg)
{
	if (ubus_thread_id <= 0)
		return;

	pthread_mutex_lock(&ubus_lock);

	ubus_last_status.status = status;

	if (!ubus_has_subscribers) {
		/* Do not send notifications if no active subscribers */
		goto end;
	}

	blob_buf_init(&b, 0);

	blobmsg_add_u32    (&b, "status",     status);
	blobmsg_add_string (&b, "status_str", get_status_string(status));
	blobmsg_add_u32    (&b, "level",      level);
	blobmsg_add_string (&b, "level_str",  get_level_string(level));
	blobmsg_add_u32    (&b, "error",      error);
	blobmsg_add_string (&b, "msg",        msg);

	/* -1 = do not wait for reply from ubusd */
	ubus_notify(ubus_ctx, &ubus_object, "info", b.head, -1);

end:
	pthread_mutex_unlock(&ubus_lock);
}
