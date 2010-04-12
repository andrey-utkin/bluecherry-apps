/*
 * Copyright (C) 2010 Bluecherry, LLC
 *
 * Confidential, all rights reserved. No distribution is permitted.
 */

#include <stdlib.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <time.h>

#include "bc-server.h"

static void update_time(struct bc_handle *bc)
{
	char buf[20];
	struct tm tm;
	time_t t;

	t = time(NULL);
	gmtime_r(&t, &tm);
	strftime(buf, 20, "%F %T", &tm);
	bc_set_osd(bc, buf);
}

static void *bc_device_thread(void *data)
{
	struct bc_rec *bc_rec = data;
	struct bc_handle *bc = bc_rec->bc;
	int ret;

	bc_log("I(%s): Starting record: %s", bc_rec->id, bc_rec->name);

	for (;;) {
		if ((ret = bc_buf_get(bc)) == EAGAIN)
			continue;
		else if (ret) {
			bc_log("error getting buffer: %m");
			/* XXX Do something */
		}
		if (bc_buf_key_frame(bc))
			update_time(bc);

		if (bc_mux_out(bc_rec)) {
			bc_log("error writing frame to outfile: %m");
			/* XXX Do something */
		}
	}

	bc_close_avcodec(bc_rec);
	bc_handle_free(bc);

	return NULL;
}

int bc_start_record(struct bc_rec *bc_rec)
{
	struct bc_handle *bc;
	int ret;

	/* Open the device */
	if ((bc = bc_handle_get(bc_rec->dev)) == NULL) {
		bc_log("E(%s): error opening device: %m", bc_rec->id);
		return -1;
	}

	bc_rec->bc = bc;

	ret = bc_set_format(bc, V4L2_PIX_FMT_MPEG, bc_rec->width,
			    bc_rec->height);

	if (ret) {
		bc_log("E(%s): error setting format: %m", bc_rec->id);
		return -1;
	}

	if (bc_handle_start(bc)) {
		bc_log("E(%s): error starting stream: %m", bc_rec->id);
		return -1;
	}
 
	if (bc_open_avcodec(bc_rec)) {
		bc_log("E(%s): error opening avcodec: %m", bc_rec->id);
		return -1;
	}

	if (pthread_create(&bc_rec->thread, NULL, bc_device_thread,
			   bc_rec) != 0) {
		bc_log("E(%s): failed to start thread: %m", bc_rec->id);
		return -1;
	}
 
	return 0;
}
