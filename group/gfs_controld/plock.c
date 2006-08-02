/******************************************************************************
*******************************************************************************
**
**  Copyright (C) 2005 Red Hat, Inc.  All rights reserved.
**
**  This copyrighted material is made available to anyone wishing to use,
**  modify, copy, or redistribute it subject to the terms and conditions
**  of the GNU General Public License v.2.
**
*******************************************************************************
******************************************************************************/

#include <sys/types.h>
#include <asm/types.h>
#include <sys/uio.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdint.h>
#include <fcntl.h>
#include <netdb.h>
#include <limits.h>
#include <unistd.h>
#include <openais/saAis.h>
#include <openais/saCkpt.h>
#include <linux/lock_dlm_plock.h>

#include "lock_dlm.h"

#define PROC_MISC               "/proc/misc"
#define PROC_DEVICES            "/proc/devices"
#define MISC_NAME               "misc"
#define CONTROL_DIR             "/dev/misc"
#define CONTROL_NAME            "lock_dlm_plock"

static int control_fd = -1;
extern int our_nodeid;
static int plocks_online = 0;

static SaCkptHandleT ckpt_handle;
static SaCkptCallbacksT callbacks = { 0, 0 };
static SaVersionT version = { 'B', 1, 1 };
static char section_buf[1024 * 1024];
static uint32_t section_len;

struct pack_plock {
	uint64_t start;
	uint64_t end;
	uint64_t owner;
	uint32_t pid;
	uint32_t nodeid;
	uint8_t ex;
	uint8_t waiter;
	uint16_t pad1;
	uint32_t pad;
};

struct resource {
	struct list_head	list;	   /* list of resources */
	uint64_t		number;
	struct list_head	locks;	  /* one lock for each range */
	struct list_head	waiters;
};

struct posix_lock {
	struct list_head	list;	   /* resource locks or waiters list */
	uint32_t		pid;
	uint64_t		owner;
	uint64_t		start;
	uint64_t		end;
	int			ex;
	int			nodeid;
};

struct lock_waiter {
	struct list_head	list;
	struct gdlm_plock_info	info;
};

static int get_proc_number(const char *file, const char *name, uint32_t *number)
{
	FILE *fl;
	char nm[256];
	int c;

	if (!(fl = fopen(file, "r"))) {
		log_error("%s: fopen failed: %s", file, strerror(errno));
		return 0;
	}

	while (!feof(fl)) {
		if (fscanf(fl, "%d %255s\n", number, &nm[0]) == 2) {
			if (!strcmp(name, nm)) {
				fclose(fl);
				return 1;
			}
		} else do {
			c = fgetc(fl);
		} while (c != EOF && c != '\n');
	}
	fclose(fl);

	log_error("%s: No entry for %s found", file, name);
	return 0;
}

static int control_device_number(uint32_t *major, uint32_t *minor)
{
	if (!get_proc_number(PROC_DEVICES, MISC_NAME, major) ||
	    !get_proc_number(PROC_MISC, GDLM_PLOCK_MISC_NAME, minor)) {
		*major = 0;
		return 0;
	}

	return 1;
}

/*
 * Returns 1 if exists; 0 if it doesn't; -1 if it's wrong
 */
static int control_exists(const char *control, uint32_t major, uint32_t minor)
{
	struct stat buf;

	if (stat(control, &buf) < 0) {
		if (errno != ENOENT)
			log_error("%s: stat failed: %s", control,
				  strerror(errno));
		return 0;
	}

	if (!S_ISCHR(buf.st_mode)) {
		log_error("%s: Wrong inode type", control);
		if (!unlink(control))
			return 0;
		log_error("%s: unlink failed: %s", control, strerror(errno));
		return -1;
	}

	if (major && buf.st_rdev != makedev(major, minor)) {
		log_error("%s: Wrong device number: (%u, %u) instead of "
			  "(%u, %u)", control, major(buf.st_mode),
			  minor(buf.st_mode), major, minor);
		if (!unlink(control))
			return 0;
		log_error("%s: unlink failed: %s", control, strerror(errno));
		return -1;
	}

	return 1;
}

static int create_control(const char *control, uint32_t major, uint32_t minor)
{
	int ret;
	mode_t old_umask;

	if (!major)
		return 0;

	old_umask = umask(0022);
	ret = mkdir(CONTROL_DIR, 0777);
	umask(old_umask);
	if (ret < 0 && errno != EEXIST) {
		log_error("%s: mkdir failed: %s", CONTROL_DIR, strerror(errno));
		return 0;
	}

	if (mknod(control, S_IFCHR | S_IRUSR | S_IWUSR, makedev(major, minor)) < 0) {
		log_error("%s: mknod failed: %s", control, strerror(errno));
		return 0;
	}

	return 1;
}

static int open_control(void)
{
	char control[PATH_MAX];
	uint32_t major = 0, minor = 0;

	if (control_fd != -1)
		return 0;

	snprintf(control, sizeof(control), "%s/%s", CONTROL_DIR, CONTROL_NAME);

	if (!control_device_number(&major, &minor)) {
		log_error("Is dlm missing from kernel?");
		return -1;
	}

	if (!control_exists(control, major, minor) &&
	    !create_control(control, major, minor)) {
		log_error("Failure to communicate with kernel lock_dlm");
		return -1;
	}

	control_fd = open(control, O_RDWR);
	if (control_fd < 0) {
		log_error("Failure to communicate with kernel lock_dlm: %s",
			  strerror(errno));
		return -1;
	}

	return 0;
}

int setup_plocks(void)
{
	SaAisErrorT err;
	int rv;

	err = saCkptInitialize(&ckpt_handle, &callbacks, &version);
	if (err == SA_AIS_OK)
		plocks_online = 1;
	else
		log_error("ckpt init error %d - plocks unavailable", err);

	/* REMOVEME: disable actual use of checkpoints for now */
	plocks_online = 0;

	rv = open_control();
	if (rv)
		return rv;

	log_debug("plocks %d", control_fd);

	return control_fd;
}

int process_plocks(void)
{
	struct mountgroup *mg;
	struct gdlm_plock_info info;
	struct gdlm_header *hd;
	char *buf;
	int len, rv;

	memset(&info, 0, sizeof(info));

	rv = read(control_fd, &info, sizeof(info));

	log_debug("process_plocks %d op %d fs %x num %llx ex %d wait %d", rv,
		  info.optype, info.fsid, info.number, info.ex, info.wait);

	mg = find_mg_id(info.fsid);
	if (!mg) {
		log_debug("process_plocks: no mg id %x", info.fsid);
		rv = -EEXIST;
		goto fail;
	}

	len = sizeof(struct gdlm_header) + sizeof(struct gdlm_plock_info);
	buf = malloc(len);
	if (!buf) {
		rv = -ENOMEM;
		goto fail;
	}
	memset(buf, 0, len);

	info.nodeid = our_nodeid;

	/* FIXME: do byte swapping */

	hd = (struct gdlm_header *)buf;
	hd->type = MSG_PLOCK;
	hd->nodeid = our_nodeid;
	hd->to_nodeid = 0;
	memcpy(buf + sizeof(struct gdlm_header), &info, sizeof(info));

	rv = send_group_message(mg, len, buf);

	free(buf);

	if (rv) {
		log_error("send plock error %d", rv);
		goto fail;
	}
	return 0;

 fail:
	info.rv = rv;
	rv = write(control_fd, &info, sizeof(info));

	return 0;
}

static struct resource *search_resource(struct mountgroup *mg, uint64_t number)
{
	struct resource *r;

	list_for_each_entry(r, &mg->resources, list) {
		if (r->number == number)
			return r;
	}
	return NULL;
}

static int find_resource(struct mountgroup *mg, uint64_t number, int create,
			 struct resource **r_out)
{
	struct resource *r = NULL;
	int rv = 0;

	r = search_resource(mg, number);
	if (r)
		goto out;

	if (create == 0) {
		rv = -ENOENT;
		goto out;
	}

	r = malloc(sizeof(struct resource));
	if (!r) {
		rv = -ENOMEM;
		goto out;
	}

	memset(r, 0, sizeof(struct resource));
	r->number = number;
	INIT_LIST_HEAD(&r->locks);
	INIT_LIST_HEAD(&r->waiters);

	list_add_tail(&r->list, &mg->resources);
 out:
	*r_out = r;
	return rv;
}

static void put_resource(struct resource *r)
{
	if (list_empty(&r->locks) && list_empty(&r->waiters)) {
		list_del(&r->list);
		free(r);
	}
}

static inline int ranges_overlap(uint64_t start1, uint64_t end1,
				 uint64_t start2, uint64_t end2)
{
	if (end1 < start2 || start1 > end2)
		return FALSE;
	return TRUE;
}

/**
 * overlap_type - returns a value based on the type of overlap
 * @s1 - start of new lock range
 * @e1 - end of new lock range
 * @s2 - start of existing lock range
 * @e2 - end of existing lock range
 *
 */

static int overlap_type(uint64_t s1, uint64_t e1, uint64_t s2, uint64_t e2)
{
	int ret;

	/*
	 * ---r1---
	 * ---r2---
	 */

	if (s1 == s2 && e1 == e2)
		ret = 0;

	/*
	 * --r1--
	 * ---r2---
	 */

	else if (s1 == s2 && e1 < e2)
		ret = 1;

	/*
	 *   --r1--
	 * ---r2---
	 */

	else if (s1 > s2 && e1 == e2)
		ret = 1;

	/*
	 *  --r1--
	 * ---r2---
	 */

	else if (s1 > s2 && e1 < e2)
		ret = 2;

	/*
	 * ---r1---  or  ---r1---  or  ---r1---
	 * --r2--	  --r2--       --r2--
	 */

	else if (s1 <= s2 && e1 >= e2)
		ret = 3;

	/*
	 *   ---r1---
	 * ---r2---
	 */

	else if (s1 > s2 && e1 > e2)
		ret = 4;

	/*
	 * ---r1---
	 *   ---r2---
	 */

	else if (s1 < s2 && e1 < e2)
		ret = 4;

	else
		ret = -1;

	return ret;
}

/* shrink the range start2:end2 by the partially overlapping start:end */

static int shrink_range2(uint64_t *start2, uint64_t *end2,
			 uint64_t start, uint64_t end)
{
	int error = 0;

	if (*start2 < start)
		*end2 = start - 1;
	else if (*end2 > end)
		*start2 =  end + 1;
	else
		error = -1;
	return error;
}

static int shrink_range(struct posix_lock *po, uint64_t start, uint64_t end)
{
	return shrink_range2(&po->start, &po->end, start, end);
}

static int is_conflict(struct resource *r, struct gdlm_plock_info *in)
{
	struct posix_lock *po;

	list_for_each_entry(po, &r->locks, list) {
		if (po->nodeid == in->nodeid && po->owner == in->owner)
			continue;
		if (!ranges_overlap(po->start, po->end, in->start, in->end))
			continue;

		if (in->ex || po->ex)
			return 1;
	}
	return 0;
}

static int add_lock(struct resource *r, uint32_t nodeid, uint64_t owner,
		    uint32_t pid, int ex, uint64_t start, uint64_t end)
{
	struct posix_lock *po;

	po = malloc(sizeof(struct posix_lock));
	if (!po)
		return -ENOMEM;
	memset(po, 0, sizeof(struct posix_lock));

	po->start = start;
	po->end = end;
	po->nodeid = nodeid;
	po->owner = owner;
	po->pid = pid;
	po->ex = ex;
	list_add_tail(&po->list, &r->locks);

	return 0;
}

/* RN within RE (and starts or ends on RE boundary)
   1. add new lock for non-overlap area of RE, orig mode
   2. convert RE to RN range and mode */

static int lock_case1(struct posix_lock *po, struct resource *r,
		      struct gdlm_plock_info *in)
{
	uint64_t start2, end2;

	/* non-overlapping area start2:end2 */
	start2 = po->start;
	end2 = po->end;
	shrink_range2(&start2, &end2, in->start, in->end);

	po->start = in->start;
	po->end = in->end;
	po->ex = in->ex;

	add_lock(r, in->nodeid, in->owner, in->pid, !in->ex, start2, end2);

	return 0;
}

/* RN within RE (RE overlaps RN on both sides)
   1. add new lock for front fragment, orig mode
   2. add new lock for back fragment, orig mode
   3. convert RE to RN range and mode */
			 
static int lock_case2(struct posix_lock *po, struct resource *r,
		      struct gdlm_plock_info *in)

{
	add_lock(r, in->nodeid, in->owner, in->pid,
		 !in->ex, po->start, in->start - 1);

	add_lock(r, in->nodeid, in->owner, in->pid,
		 !in->ex, in->end + 1, po->end);

	po->start = in->start;
	po->end = in->end;
	po->ex = in->ex;

	return 0;
}

static int lock_internal(struct mountgroup *mg, struct resource *r,
			 struct gdlm_plock_info *in)
{
	struct posix_lock *po, *safe;
	int rv = 0;

	list_for_each_entry_safe(po, safe, &r->locks, list) {
		if (po->nodeid != in->nodeid || po->owner != in->owner)
			continue;
		if (!ranges_overlap(po->start, po->end, in->start, in->end))
			continue;

		/* existing range (RE) overlaps new range (RN) */

		switch(overlap_type(in->start, in->end, po->start, po->end)) {

		case 0:
			if (po->ex == in->ex)
				goto out;

			/* ranges the same - just update the existing lock */
			po->ex = in->ex;
			goto out;

		case 1:
			if (po->ex == in->ex)
				goto out;

			rv = lock_case1(po, r, in);
			goto out;

		case 2:
			if (po->ex == in->ex)
				goto out;

			rv = lock_case2(po, r, in);
			goto out;

		case 3:
			list_del(&po->list);
			free(po);
			break;

		case 4:
			if (po->start < in->start)
				po->end = in->start - 1;
			else
				po->start = in->end + 1;
			break;

		default:
			rv = -1;
			goto out;
		}
	}

	rv = add_lock(r, in->nodeid, in->owner, in->pid,
		      in->ex, in->start, in->end);

 out:
	return rv;

}

static int unlock_internal(struct mountgroup *mg, struct resource *r,
			   struct gdlm_plock_info *in)
{
	struct posix_lock *po, *safe;
	int rv = 0;

	list_for_each_entry_safe(po, safe, &r->locks, list) {
		if (po->nodeid != in->nodeid || po->owner != in->owner)
			continue;
		if (!ranges_overlap(po->start, po->end, in->start, in->end))
			continue;

		/* existing range (RE) overlaps new range (RN) */

		switch(overlap_type(in->start, in->end, po->start, po->end)) {

		case 0:
			/* ranges the same - just remove the existing lock */

			list_del(&po->list);
			free(po);
			goto out;

		case 1:
			/* RN within RE and starts or ends on RE boundary -
			 * shrink and update RE */

			shrink_range(po, in->start, in->end);
			goto out;

		case 2:
			/* RN within RE - shrink and update RE to be front
			 * fragment, and add a new lock for back fragment */

			add_lock(r, in->nodeid, in->owner, in->pid,
				 po->ex, in->end + 1, po->end);
			po->end = in->start - 1;
			goto out;

		case 3:
			/* RE within RN - remove RE, then continue checking
			 * because RN could cover other locks */

			list_del(&po->list);
			free(po);
			continue;

		case 4:
			/* front of RE in RN, or end of RE in RN - shrink and
			 * update RE, then continue because RN could cover
			 * other locks */

			shrink_range(po, in->start, in->end);
			continue;

		default:
			rv = -1;
			goto out;
		}
	}

 out:
	return rv;
}

static int add_waiter(struct mountgroup *mg, struct resource *r,
		      struct gdlm_plock_info *in)

{
	struct lock_waiter *w;
	w = malloc(sizeof(struct lock_waiter));
	if (!w)
		return -ENOMEM;
	memcpy(&w->info, in, sizeof(struct gdlm_plock_info));
	list_add_tail(&w->list, &r->waiters);
	return 0;
}

static void do_waiters(struct mountgroup *mg, struct resource *r)
{
	struct lock_waiter *w, *safe;
	struct gdlm_plock_info *in;
	int rv;

	list_for_each_entry_safe(w, safe, &r->waiters, list) {
		in = &w->info;

		if (is_conflict(r, in))
			continue;

		list_del(&w->list);

		rv = lock_internal(mg, r, in);

		free(w);
	}
}

static int do_lock(struct mountgroup *mg, struct gdlm_plock_info *in)
{
	struct resource *r = NULL;
	int rv;

	rv = find_resource(mg, in->number, 1, &r);
	if (rv || !r)
		goto out;

	if (is_conflict(r, in)) {
		if (!in->wait)
			rv = -EAGAIN;
		else
			rv = add_waiter(mg, r, in);
		goto out;
	}

	rv = lock_internal(mg, r, in);
	if (rv)
		goto out;

	do_waiters(mg, r);
	put_resource(r);
 out:
	return rv;
}

static int do_unlock(struct mountgroup *mg, struct gdlm_plock_info *in)
{
	struct resource *r = NULL;
	int rv;

	rv = find_resource(mg, in->number, 0, &r);
	if (rv || !r)
		goto out;

	rv = unlock_internal(mg, r, in);
	if (rv)
		goto out;

	do_waiters(mg, r);
	put_resource(r);
 out:
	return rv;
}

void receive_plock(struct mountgroup *mg, char *buf, int len, int from)
{
	struct gdlm_plock_info info;
	struct gdlm_header *hd = (struct gdlm_header *) buf;
	int rv = 0;

	memcpy(&info, buf + sizeof(struct gdlm_header), sizeof(info));

	/* FIXME: do byte swapping */

	log_group(mg, "receive_plock from %d op %d fs %x num %llx ex %d w %d",
		  from, info.optype, info.fsid, info.number, info.ex,
		  info.wait);

	if (from != hd->nodeid || from != info.nodeid) {
		log_error("receive_plock from %d header %d info %d",
			  from, hd->nodeid, info.nodeid);
		rv = -EINVAL;
		goto out;
	}

	if (info.optype == GDLM_PLOCK_OP_GET && from != our_nodeid)
		return;

	switch (info.optype) {
	case GDLM_PLOCK_OP_LOCK:
		mg->last_plock_time = time(NULL);
		rv = do_lock(mg, &info);
		break;
	case GDLM_PLOCK_OP_UNLOCK:
		mg->last_plock_time = time(NULL);
		rv = do_unlock(mg, &info);
		break;
	case GDLM_PLOCK_OP_GET:
		/* rv = do_get(mg, &info); */
		break;
	default:
		rv = -EINVAL;
	}

 out:
	if (from == our_nodeid) {
		info.rv = rv;
		rv = write(control_fd, &info, sizeof(info));
	}
}

void plock_exit(void)
{
	if (plocks_online)
		saCkptFinalize(ckpt_handle);
}

void pack_section_buf(struct mountgroup *mg, struct resource *r)
{
	struct pack_plock *pp;
	struct posix_lock *po;
	struct lock_waiter *w;
	int count = 0;

	memset(&section_buf, 0, sizeof(section_buf));

	pp = (struct pack_plock *) &section_buf;

	list_for_each_entry(po, &r->locks, list) {
		pp->start	= po->start;
		pp->end		= po->end;
		pp->pid		= po->pid;
		pp->nodeid	= po->nodeid;
		pp->ex		= po->ex;
		pp->waiter	= 0;
		pp++;
		count++;
	}

	list_for_each_entry(w, &r->waiters, list) {
		pp->start	= w->info.start;
		pp->end		= w->info.end;
		pp->pid		= w->info.pid;
		pp->nodeid	= w->info.nodeid;
		pp->ex		= w->info.ex;
		pp->waiter	= 1;
		pp++;
		count++;
	}

	section_len = count * sizeof(struct pack_plock);

	log_group(mg, "pack %llx count %d", r->number, count);
}

int unpack_section_buf(struct mountgroup *mg, char *numbuf, int buflen)
{
	struct pack_plock *pp;
	struct posix_lock *po;
	struct lock_waiter *w;
	struct resource *r;
	int count = section_len / sizeof(struct pack_plock);
	int i;

	r = malloc(sizeof(struct resource));
	if (!r)
		return -ENOMEM;
	memset(r, 0, sizeof(struct resource));

	sscanf(numbuf, "%llu", &r->number);

	log_group(mg, "unpack %llx count %d", r->number, count);

	pp = (struct pack_plock *) &section_buf;

	for (i = 0; i < count; i++) {
		if (!pp->waiter) {
			po = malloc(sizeof(struct posix_lock));
			po->start	= pp->start;
			po->end		= pp->end;
			po->pid		= pp->pid;
			po->ex		= pp->ex;
			list_add_tail(&po->list, &r->locks);
		} else {
			w = malloc(sizeof(struct lock_waiter));
			w->info.start	= pp->start;
			w->info.end	= pp->end;
			w->info.pid	= pp->pid;
			w->info.nodeid	= pp->nodeid;
			w->info.ex	= pp->ex;
			list_add_tail(&w->list, &r->waiters);
		}
		pp++;
	}

	list_add_tail(&r->list, &mg->resources);
	return 0;
}

/* copy all plock state into a checkpoint so new node can retrieve it */

void store_plocks(struct mountgroup *mg)
{
	SaCkptCheckpointCreationAttributesT attr;
	SaCkptCheckpointHandleT h;
	SaCkptSectionIdT section_id;
	SaCkptSectionCreationAttributesT section_attr;
	SaNameT name;
	SaAisErrorT rv;
	char buf[32];
	struct resource *r;
	struct posix_lock *po;
	struct lock_waiter *w;
	int len, r_count, total_size, section_size, max_section_size;

	if (!plocks_online)
		return;

	/* no change to plock state since we created the last checkpoint */
	if (mg->last_checkpoint_time > mg->last_plock_time) {
		log_group(mg, "store_plocks: ckpt uptodate");
		return;
	}
	mg->last_checkpoint_time = time(NULL);

	len = snprintf(name.value, SA_MAX_NAME_LENGTH, "gfsplock.%s", mg->name);
	name.length = len;

	/* unlink an old checkpoint before we create a new one */
	if (mg->cp_handle) {
		log_group(mg, "store_plocks: unlink ckpt");
		h = (SaCkptCheckpointHandleT) mg->cp_handle;
		rv = saCkptCheckpointUnlink(h, &name);
		if (rv != SA_AIS_OK)
			log_error("ckpt unlink error %d %s", rv, mg->name);
		h = 0;
		mg->cp_handle = 0;
	}

	/* loop through all plocks to figure out sizes to set in
	   the attr fields */

	r_count = 0;
	total_size = 0;
	max_section_size = 0;

	list_for_each_entry(r, &mg->resources, list) {
		r_count++;
		section_size = 0;
		list_for_each_entry(po, &r->locks, list)
			section_size += sizeof(struct pack_plock);
		list_for_each_entry(w, &r->waiters, list)
			section_size += sizeof(struct pack_plock);
		total_size += section_size;
		if (section_size > max_section_size)
			max_section_size = section_size;
	}

	log_group(mg, "store_plocks: r_count %d total %d max_section %d",
		  r_count, total_size, max_section_size);

	attr.creationFlags = SA_CKPT_WR_ALL_REPLICAS;
	attr.checkpointSize = total_size;
	attr.retentionDuration = SA_TIME_MAX;
	attr.maxSections = r_count;
	attr.maxSectionSize = max_section_size;
	attr.maxSectionIdSize = 21;             /* 20 digits in max uint64 */

 open_retry:
	rv = saCkptCheckpointOpen(ckpt_handle, &name, &attr,
				  SA_CKPT_CHECKPOINT_CREATE |
				  SA_CKPT_CHECKPOINT_READ |
				  SA_CKPT_CHECKPOINT_WRITE,
				  0, &h);
	if (rv == SA_AIS_ERR_TRY_AGAIN) {
		log_group(mg, "store_plocks: ckpt open retry");
		sleep(1);
		goto open_retry;
	}
	if (rv != SA_AIS_OK) {
		log_error("store_plocks: ckpt open error %d %s", rv, mg->name);
		return;
	}

	mg->cp_handle = (uint64_t) h;

	list_for_each_entry(r, &mg->resources, list) {
		memset(&buf, 0, 32);
		len = snprintf(buf, 32, "%llu", r->number);

		section_id.id = buf;
		section_id.idLen = len + 1;
		section_attr.sectionId = &section_id;
		section_attr.expirationTime = SA_TIME_END;

		pack_section_buf(mg, r);

 create_retry:
		rv = saCkptSectionCreate(h, &section_attr, &section_buf,
					 section_len);
		if (rv == SA_AIS_ERR_TRY_AGAIN) {
			log_group(mg, "store_plocks: ckpt create retry");
			sleep(1);
			goto create_retry;
		}
		if (rv != SA_AIS_OK) {
			log_error("store_plocks: ckpt create error %d %s",
				  rv, mg->name);
			break;
		}
	}
}

/* called by a node that's just been added to the group to get existing plock
   state */

void retrieve_plocks(struct mountgroup *mg)
{
	SaCkptCheckpointHandleT h;
	SaCkptSectionIterationHandleT itr;
	SaCkptSectionDescriptorT desc;
	SaCkptIOVectorElementT iov;
	SaNameT name;
	SaAisErrorT rv;
	int len;

	if (!plocks_online)
		return;

	len = snprintf(name.value, SA_MAX_NAME_LENGTH, "gfsplock.%s", mg->name);
	name.length = len;

 open_retry:
	rv = saCkptCheckpointOpen(ckpt_handle, &name, NULL,
				  SA_CKPT_CHECKPOINT_READ, 0, &h);
	if (rv == SA_AIS_ERR_TRY_AGAIN) {
		log_group(mg, "retrieve_plocks: ckpt open retry");
		sleep(1);
		goto open_retry;
	}
	if (rv != SA_AIS_OK) {
		log_error("retrieve_plocks: ckpt open error %d %s",
			  rv, mg->name);
		return;
	}

 init_retry:
	rv = saCkptSectionIterationInitialize(h, SA_CKPT_SECTIONS_ANY, 0, &itr);
	if (rv == SA_AIS_ERR_TRY_AGAIN) {
		log_group(mg, "retrieve_plocks: ckpt iterinit retry");
		sleep(1);
		goto init_retry;
	}
	if (rv != SA_AIS_OK) {
		log_error("retrieve_plocks: ckpt iterinit error %d %s",
			  rv, mg->name);
		return;
	}

	while (1) {
 next_retry:
		rv = saCkptSectionIterationNext(itr, &desc);
		if (rv == SA_AIS_ERR_NO_SECTIONS)
			break;
		if (rv == SA_AIS_ERR_TRY_AGAIN) {
			log_group(mg, "retrieve_plocks: ckpt iternext retry");
			sleep(1);
			goto next_retry;
		}
		if (rv != SA_AIS_OK) {
			log_error("retrieve_plocks: ckpt iternext error %d %s",
				  rv, mg->name);
			break;
		}

		iov.sectionId = desc.sectionId;
		iov.dataBuffer = &section_buf;
		iov.dataSize = desc.sectionSize;
		iov.dataOffset = 0;

 read_retry:
		rv = saCkptCheckpointRead(h, &iov, 1, NULL);
		if (rv == SA_AIS_ERR_TRY_AGAIN) {
			log_group(mg, "retrieve_plocks: ckpt read retry");
			sleep(1);
			goto read_retry;
		}
		if (rv != SA_AIS_OK) {
			log_error("retrieve_plocks: ckpt read error %d %s",
				  rv, mg->name);
			break;
		}

		unpack_section_buf(mg, desc.sectionId.id, desc.sectionId.idLen);
	}

	saCkptSectionIterationFinalize(itr);
	saCkptCheckpointClose(h);
}

int dump_plocks(char *name, int fd)
{
	struct mountgroup *mg;
	struct posix_lock *po;
	struct lock_waiter *w;
	struct resource *r;
	char line[MAXLINE];
	int rv;

	if (!name)
		return -1;

	mg = find_mg(name);
	if (!mg)
		return -1;

	list_for_each_entry(r, &mg->resources, list) {

		list_for_each_entry(po, &r->locks, list) {
			snprintf(line, MAXLINE,
			      "%llu %s %llu-%llu nodeid %d pid %u owner %llx\n",
			      r->number,
			      po->ex ? "WR" : "RD",
			      po->start, po->end,
			      po->nodeid, po->pid, po->owner);

			rv = write(fd, line, strlen(line));
		}

		list_for_each_entry(w, &r->waiters, list) {
			snprintf(line, MAXLINE,
			      "%llu WAITING %s %llu-%llu nodeid %d pid %u owner %llx\n",
			      r->number,
			      po->ex ? "WR" : "RD",
			      po->start, po->end,
			      po->nodeid, po->pid, po->owner);

			rv = write(fd, line, strlen(line));
		}
	}

	return 0;
}
