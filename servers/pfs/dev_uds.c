/*
 * Unix Domain Sockets Implementation (PF_UNIX, PF_LOCAL)
 * This code handles requests generated by operations on /dev/uds
 *
 * The entry points into this file are...
 *
 *   uds_open:   handles the   open(2) syscall on /dev/uds
 *   uds_close:  handles the  close(2) syscall on /dev/uds
 *   uds_select: handles the select(2) syscall on /dev/uds
 *   uds_read:   handles the   read(2) syscall on /dev/uds
 *   uds_write:  handles the  write(2) syscall on /dev/uds
 *   uds_ioctl:  handles the  ioctl(2) syscall on /dev/uds
 *   uds_status: handles status requests.
 *   uds_cancel: handles cancelled syscalls.
 *
 * Also See...
 *
 *   table.c, uds.c, uds.h
 *
 * Overview
 *
 * The interface to unix domain sockets is similar to the
 * the interface to network sockets. There is a character
 * device (/dev/uds) that uses STYLE_CLONE and this server
 * is a 'driver' for that device.
 */

#define DEBUG 0

#include "inc.h"
#include "const.h"
#include "glo.h"
#include "uds.h"

static int uds_perform_read(int minor, endpoint_t m_source, size_t
	size, int pretend);
static int uds_perform_write(int minor, endpoint_t m_source, size_t
	size, int pretend);

int uds_open(message *dev_m_in, message *dev_m_out)
{
	message fs_m_in, fs_m_out;
	struct uucred ucred;
	int rc, i;
	int minor;

#if DEBUG == 1
	static int call_count = 0;
	printf("(uds) [%d] uds_open() call_count=%d\n", uds_minor(dev_m_in),
							++call_count);
	printf("Endpoint: 0x%x\n", dev_m_in->USER_ENDPT);
#endif

	/*
	 * Find a slot in the descriptor table for the new descriptor.
	 * The index of the descriptor in the table will be returned.
	 * Subsequent calls to read/write/close/ioctl/etc will use this
	 * minor number. The minor number must be different from the
	 * the /dev/uds device's minor number (currently 0).
	 */

	minor = -1; /* to trap error */

	for (i = 1; i < NR_FDS; i++) {
		if (uds_fd_table[i].state == UDS_FREE) {
			minor = i;
			break;
		}
	}

	if (minor == -1) {

		/* descriptor table full */
		uds_set_reply(dev_m_out, DEV_OPEN_REPL, dev_m_in->USER_ENDPT,
			      (cp_grant_id_t) dev_m_in->IO_GRANT, ENFILE);
		return ENFILE;
	}

	/*
	 * We found a slot in uds_fd_table, now initialize the descriptor
	 */

	/* mark this one as 'in use' so that it doesn't get assigned to
	 * another socket
	 */
	uds_fd_table[minor].state = UDS_INUSE;

	/* track the system call we are performing in case it gets cancelled */
	uds_fd_table[minor].call_nr = dev_m_in->m_type;
	uds_fd_table[minor].ioctl = 0;
	uds_fd_table[minor].syscall_done = 0;

	/* set the socket owner */
	uds_fd_table[minor].owner = dev_m_in->USER_ENDPT;
	uds_fd_table[minor].endpoint = dev_m_in->USER_ENDPT;

	/* setup select(2) framework */
	uds_fd_table[minor].selecting = 0;
	uds_fd_table[minor].select_proc = 0;
	uds_fd_table[minor].sel_ops_in = 0;
	uds_fd_table[minor].sel_ops_out = 0;
	uds_fd_table[minor].status_updated = 0;

	/* initialize the data pointer (pos) to the start of the PIPE */
	uds_fd_table[minor].pos = 0;

	/* the PIPE is initially empty */
	uds_fd_table[minor].size = 0;

	/* the default for a new socket is to allow reading and writing.
	 * shutdown(2) will remove one or both flags.
	 */
	uds_fd_table[minor].mode = S_IRUSR|S_IWUSR;

	/* In libc socket(2) sets this to the actual value later with the
	 * NWIOSUDSTYPE ioctl().
	 */
	uds_fd_table[minor].type = -1;

	/* Clear the backlog by setting each entry to -1 */
	for (i = 0; i < UDS_SOMAXCONN; i++) {
		/* initially no connections are pending */
		uds_fd_table[minor].backlog[i] = -1;
	}

	memset(&uds_fd_table[minor].ancillary_data, '\0', sizeof(struct
								ancillary));
	for (i = 0; i < OPEN_MAX; i++) {
		uds_fd_table[minor].ancillary_data.fds[i] = -1;
	}

	/* default the size to UDS_SOMAXCONN */
	uds_fd_table[minor].backlog_size = UDS_SOMAXCONN;

	/* the socket isn't listening for incoming connections until
	 * listen(2) is called
	 */
	uds_fd_table[minor].listening = 0;

	/* initially the socket is not connected to a peer */
	uds_fd_table[minor].peer = -1;

	/* there isn't a child waiting to be accept(2)'d */
	uds_fd_table[minor].child = -1;

	/* initially the socket is not bound or listening on an address */
	memset(&(uds_fd_table[minor].addr), '\0', sizeof(struct sockaddr_un));
	memset(&(uds_fd_table[minor].source), '\0', sizeof(struct sockaddr_un));
	memset(&(uds_fd_table[minor].target), '\0', sizeof(struct sockaddr_un));

	/* Initially the socket isn't suspended. */
	uds_fd_table[minor].suspended = UDS_NOT_SUSPENDED;

	/* and the socket doesn't have an I/O grant initially */
	uds_fd_table[minor].io_gr = (cp_grant_id_t) 0;

	/* since there is no I/O grant it effectively has no size either */
	uds_fd_table[minor].io_gr_size = 0;

	/* The process isn't suspended so we don't flag it as revivable */
	uds_fd_table[minor].ready_to_revive = 0;

	/* get the effective user id and effective group id from the endpoint */
	/* this is needed in the REQ_NEWNODE request to PFS. */
	rc = getnucred(uds_fd_table[minor].endpoint, &ucred);
	if (rc == -1) {
		/* roll back the changes we made to the descriptor */
		memset(&(uds_fd_table[minor]), '\0', sizeof(uds_fd_t));

		/* likely error: invalid endpoint / proc doesn't exist */
		uds_set_reply(dev_m_out, DEV_OPEN_REPL, dev_m_in->USER_ENDPT,
			      (cp_grant_id_t) dev_m_in->IO_GRANT, errno);
		return errno;
	}

	/* Prepare Request to the FS side of PFS */

	fs_m_in.m_type = REQ_NEWNODE;
	fs_m_in.REQ_MODE = I_NAMED_PIPE;
	fs_m_in.REQ_DEV = NO_DEV;
	fs_m_in.REQ_UID = ucred.cr_uid;
	fs_m_in.REQ_GID = ucred.cr_gid;

	/* Request a new inode on the pipe file system */

	rc = fs_newnode(&fs_m_in, &fs_m_out);
	if (rc != OK) {
		/* roll back the changes we made to the descriptor */
		memset(&(uds_fd_table[minor]), '\0', sizeof(uds_fd_t));

		/* likely error: get_block() failed */
		uds_set_reply(dev_m_out, DEV_OPEN_REPL, dev_m_in->USER_ENDPT,
				(cp_grant_id_t) dev_m_in->IO_GRANT, rc);
		return rc;
	}

	/* Process the response */

	uds_fd_table[minor].inode_nr = fs_m_out.RES_INODE_NR;

	/* prepare the reply */

	uds_fd_table[minor].syscall_done = 1;
	uds_set_reply(dev_m_out, DEV_OPEN_REPL, dev_m_in->USER_ENDPT,
		      (cp_grant_id_t) dev_m_in->IO_GRANT, minor);
	return minor;
}

int uds_close(message *dev_m_in, message *dev_m_out)
{
	int minor;
	message fs_m_in, fs_m_out;
	int rc;

#if DEBUG == 1
	static int call_count = 0;
	printf("(uds) [%d] uds_close() call_count=%d\n", uds_minor(dev_m_in),
							++call_count);
	printf("Endpoint: 0x%x\n", dev_m_in->USER_ENDPT);
#endif

	minor = uds_minor(dev_m_in);

	if (uds_fd_table[minor].state != UDS_INUSE) {
		/* attempted to close a socket that hasn't been opened --
		 * something is very wrong :(
		 */
		uds_set_reply(dev_m_out, DEV_CLOSE_REPL, dev_m_in->USER_ENDPT,
			      (cp_grant_id_t) dev_m_in->IO_GRANT, EINVAL);
		return EINVAL;
	}

	/* no need to track the syscall in case of cancellation. close() is
	 * atomic and can't be cancelled. no need to update the endpoint here,
	 * we won't be needing it to kill the socket
	 */

	/* if the socket is connected, disconnect it */
	if (uds_fd_table[minor].peer != -1) {
		int peer = uds_fd_table[minor].peer;

		/* set peer of this peer to -1 */
		uds_fd_table[peer].peer = -1;

		/* error to pass to peer */
		uds_fd_table[peer].err = ECONNRESET;

		/* if peer was blocked on I/O revive peer */
		if (uds_fd_table[peer].suspended) {
			uds_fd_table[peer].ready_to_revive = 1;
			uds_unsuspend(dev_m_in->m_source, peer);
		}
	}

	if (uds_fd_table[minor].ancillary_data.nfiledes > 0) {
		clear_fds(minor, &(uds_fd_table[minor].ancillary_data));
	}

	/* Prepare Request to the FS side of PFS */

	fs_m_in.m_type = REQ_PUTNODE;
	fs_m_in.REQ_INODE_NR = uds_fd_table[minor].inode_nr;
	fs_m_in.REQ_COUNT = 1;

	/* set the socket back to its original UDS_FREE state */
	memset(&(uds_fd_table[minor]), '\0', sizeof(uds_fd_t));

	/* Request the removal of the inode from the pipe file system */

	rc = fs_putnode(&fs_m_in, &fs_m_out);
	if (rc != OK) {
		perror("fs_putnode");
		/* likely error: get_block() failed */
		return rc;
	}

	uds_set_reply(dev_m_out, DEV_CLOSE_REPL, dev_m_in->USER_ENDPT,
		      (cp_grant_id_t) dev_m_in->IO_GRANT, OK);
	return OK;
}

int uds_select(message *dev_m_in, message *dev_m_out)
{
	int i, bytes;
	int minor;

#if DEBUG == 1
	static int call_count = 0;
	printf("(uds) [%d] uds_select() call_count=%d\n", uds_minor(dev_m_in),
							++call_count);
	printf("Endpoint: 0x%x\n", dev_m_in->USER_ENDPT);
#endif

	minor = uds_minor(dev_m_in);

	if (uds_fd_table[minor].state != UDS_INUSE) {

		/* attempted to close a socket that hasn't been opened --
		 * something is very wrong :(
		 */

		uds_sel_reply(dev_m_out, DEV_SEL_REPL1, minor, EINVAL);
		return EINVAL;
	}

	/* setup select(2) framework */
	uds_fd_table[minor].selecting = 1;
	uds_fd_table[minor].select_proc = dev_m_in->m_source;

	/* track the system call we are performing in case it gets cancelled */
	uds_fd_table[minor].call_nr = dev_m_in->m_type;
	uds_fd_table[minor].ioctl = 0;
	uds_fd_table[minor].syscall_done = 0;

	/* Can't update the process endpoint here, no info.  */

	uds_fd_table[minor].sel_ops_in = dev_m_in->USER_ENDPT;
	uds_fd_table[minor].sel_ops_out = 0;

	/* check if there is data available to read */
	bytes = uds_perform_read(minor, dev_m_in->m_source, 1, 1);
	if (bytes > 0) {

		/* there is data in the pipe for us to read */
		uds_fd_table[minor].sel_ops_out |= SEL_RD;

	} else if (uds_fd_table[minor].listening == 1) {

		/* check for pending connections */
		for (i = 0; i < uds_fd_table[minor].backlog_size; i++) {
			if (uds_fd_table[minor].backlog[i] != -1) {
				uds_fd_table[minor].sel_ops_out |= SEL_RD;
				break;
			}
		}
	} else if (bytes != SUSPEND) {
		uds_fd_table[minor].sel_ops_out |= SEL_RD;
	}

	/* check if we can write without blocking */
	bytes = uds_perform_write(minor, dev_m_in->m_source, PIPE_BUF, 1);
	if (bytes != 0 && bytes != SUSPEND) {
		/* There is room to write or there is an error condition */
		uds_fd_table[minor].sel_ops_out |= SEL_WR;
	}

	uds_fd_table[minor].syscall_done = 1;
	uds_sel_reply(dev_m_out, DEV_SEL_REPL1, minor,
		      uds_fd_table[minor].sel_ops_out);

	return uds_fd_table[minor].sel_ops_out;
}

static int uds_perform_read(int minor, endpoint_t m_source,
	size_t size, int pretend)
{
	int rc, peer;
	message fs_m_in;
	message fs_m_out;

#if DEBUG == 1
	static int call_count = 0;
	printf("(uds) [%d] uds_perform_read() call_count=%d\n", minor,
							++call_count);
#endif

	peer = uds_fd_table[minor].peer;

	if (size <= 0) {
		return 0;
	}

	/* check if we are allowed to read */
	if (!(uds_fd_table[minor].mode & S_IRUSR)) {

		/* socket is shutdown for reading */
		return EPIPE;
	}

	/* skip reads and writes of 0 (or less!) bytes */
	if (uds_fd_table[minor].size == 0) {

		if (peer == -1) {
			/* We're not connected. That's only a problem when this
			 * socket is connection oriented. */
			if (uds_fd_table[minor].type == SOCK_STREAM ||
			    uds_fd_table[minor].type == SOCK_SEQPACKET) {
				if (uds_fd_table[minor].err == ECONNRESET) {
					uds_fd_table[minor].err = 0;
					return ECONNRESET;
				} else {
					return ENOTCONN;
				}
			}
		}

		/* Check if process is reading from a closed pipe */
		if (peer != -1 && !(uds_fd_table[peer].mode & S_IWUSR) &&
			uds_fd_table[minor].size == 0) {
			return 0;
		}


		if (pretend) {
			return SUSPEND;
		}

		/* maybe a process is blocked waiting to write? if
		 * needed revive the writer
		 */
		if (peer != -1 && uds_fd_table[peer].suspended) {
			uds_fd_table[peer].ready_to_revive = 1;
			uds_unsuspend(m_source, peer);
		}

#if DEBUG == 1
		printf("(uds) [%d] suspending read request\n", minor);
#endif
		/* Process is reading from an empty pipe,
		 * suspend it so some bytes can be written
		 */
		uds_fd_table[minor].suspended = UDS_SUSPENDED_READ;
		return SUSPEND;
	}

	if (pretend) {

		return (size > uds_fd_table[minor].size) ?
				uds_fd_table[minor].size : size;
	}


	/* Prepare Request to the FS side of PFS */
	fs_m_in.m_type = REQ_READ;
	fs_m_in.REQ_INODE_NR = uds_fd_table[minor].inode_nr;
	fs_m_in.REQ_GRANT = uds_fd_table[minor].io_gr;
	fs_m_in.REQ_SEEK_POS_HI = 0;
	fs_m_in.REQ_SEEK_POS_LO = uds_fd_table[minor].pos;
	fs_m_in.REQ_NBYTES = (size > uds_fd_table[minor].size) ?
				uds_fd_table[minor].size : size;

	/* perform the read */
	rc = fs_readwrite(&fs_m_in, &fs_m_out);
	if (rc != OK) {
		perror("fs_readwrite");
		return rc;
	}

	/* Process the response */
#if DEBUG == 1
	printf("(uds) [%d] read complete\n", minor);
#endif

	/* move the position of the data pointer up to data we haven't
	 * read yet
	 */
	uds_fd_table[minor].pos += fs_m_out.RES_NBYTES;

	/* decrease the number of unread bytes */
	uds_fd_table[minor].size -= fs_m_out.RES_NBYTES;

	/* if we have 0 unread bytes, move the data pointer back to the
	 * start of the buffer
	 */
	if (uds_fd_table[minor].size == 0) {
		uds_fd_table[minor].pos = 0;
	}

	/* maybe a big write was waiting for us to read some data, if
	 * needed revive the writer
	 */
	if (peer != -1 && uds_fd_table[peer].suspended) {
		uds_fd_table[peer].ready_to_revive = 1;
		uds_unsuspend(m_source, peer);
	}

	/* see if peer is blocked on select() and a write is possible
	 * (from peer to minor)
	 */
	if (peer != -1 && uds_fd_table[peer].selecting == 1 &&
	    (uds_fd_table[minor].size+uds_fd_table[minor].pos + 1 < PIPE_BUF)){

		/* if the peer wants to know about write being possible
		 * and it doesn't know about it already, then let the peer know.
		 */
		if (peer != -1 && (uds_fd_table[peer].sel_ops_in & SEL_WR) &&
		    !(uds_fd_table[peer].sel_ops_out & SEL_WR)) {

			/* a write on peer is possible now */
			uds_fd_table[peer].sel_ops_out |= SEL_WR;
			uds_fd_table[peer].status_updated = 1;
			uds_unsuspend(m_source, peer);
		}
	}

	return fs_m_out.RES_NBYTES; /* return number of bytes read */
}

static int uds_perform_write(int minor, endpoint_t m_source,
						size_t size, int pretend)
{
	int rc, peer, i;
	message fs_m_in;
	message fs_m_out;

#if DEBUG == 1
	static int call_count = 0;
	printf("(uds) [%d] uds_perform_write() call_count=%d\n", minor,
							++call_count);
#endif

	/* skip reads and writes of 0 (or less!) bytes */
	if (size <= 0) {
		return 0;
	}

	/* check if we are allowed to write */
	if (!(uds_fd_table[minor].mode & S_IWUSR)) {

		/* socket is shutdown for writing */
		return EPIPE;
	}

	if (size > PIPE_BUF) {

		/* message is too big to ever write to the PIPE */
		return EMSGSIZE;
	}

	if (uds_fd_table[minor].type == SOCK_STREAM ||
			uds_fd_table[minor].type == SOCK_SEQPACKET) {

		/* if we're writing with a connection oriented socket,
		 * then it needs a peer to write to
		 */
		if (uds_fd_table[minor].peer == -1) {
			if (uds_fd_table[minor].err == ECONNRESET) {

				uds_fd_table[minor].err = 0;
				return ECONNRESET;
			} else {
				return ENOTCONN;
			}
		} else {

			peer = uds_fd_table[minor].peer;
		}

	} else /* uds_fd_table[minor].type == SOCK_DGRAM */ {

		peer = -1;

		/* locate the "peer" we want to write to */
		for (i = 0; i < NR_FDS; i++) {

			/* look for a SOCK_DGRAM socket that is bound on
			 * the target address
			 */
			if (uds_fd_table[i].type == SOCK_DGRAM &&
				uds_fd_table[i].addr.sun_family == AF_UNIX &&
				!strncmp(uds_fd_table[minor].target.sun_path,
				uds_fd_table[i].addr.sun_path, UNIX_PATH_MAX)) {

				peer = i;
				break;
			}
		}
	}

	if (peer == -1) {
		if (pretend)
			return SUSPEND;

		return ENOENT;
	}

	/* check if we write to a closed pipe */
	if (!(uds_fd_table[peer].mode & S_IRUSR)) {
		return EPIPE;
	}

	/* we have to preserve the boundary for DGRAM. if there's
	 * already a packet waiting, discard it silently and pretend
	 * it was written.
	 */
	if(uds_fd_table[minor].type == SOCK_DGRAM &&
		uds_fd_table[peer].size > 0) {
		return size;
	}

	/* check if write would overrun buffer. check if message
	 * SEQPACKET wouldn't write to an empty buffer. check if
	 * connectionless sockets have a target to write to.
	 */
	if ((uds_fd_table[peer].pos+uds_fd_table[peer].size+size > PIPE_BUF) ||
		((uds_fd_table[minor].type == SOCK_SEQPACKET) &&
		uds_fd_table[peer].size > 0)) {

		if (pretend) {
			return SUSPEND;
		}

		/* if needed revive the reader */
		if (uds_fd_table[peer].suspended) {
			uds_fd_table[peer].ready_to_revive = 1;
			uds_unsuspend(m_source, peer);
		}

#if DEBUG == 1
	printf("(uds) [%d] suspending write request\n", minor);
#endif

		/* Process is reading from an empty pipe,
		 * suspend it so some bytes can be written
		 */
		uds_fd_table[minor].suspended = UDS_SUSPENDED_WRITE;
		return SUSPEND;
	}

	if (pretend) {
		return size;
	}

	/* Prepare Request to the FS side of PFS */
	fs_m_in.m_type = REQ_WRITE;
	fs_m_in.REQ_INODE_NR = uds_fd_table[peer].inode_nr;
	fs_m_in.REQ_GRANT = uds_fd_table[minor].io_gr;
	fs_m_in.REQ_SEEK_POS_HI = 0;
	fs_m_in.REQ_SEEK_POS_LO = uds_fd_table[peer].pos +
					uds_fd_table[peer].size;
	fs_m_in.REQ_NBYTES = size;

	/* Request the write */
	rc = fs_readwrite(&fs_m_in, &fs_m_out);
	if (rc != OK) {
		perror("fs_readwrite");
		return rc;
	}

	/* Process the response */
#if DEBUG == 1
	printf("(uds) [%d] write complete\n", minor);
#endif
	/* increase the count of unread bytes */
	uds_fd_table[peer].size += fs_m_out.RES_NBYTES;


	/* fill in the source address to be returned by recvfrom & recvmsg */
	if (uds_fd_table[minor].type == SOCK_DGRAM) {
		memcpy(&uds_fd_table[peer].source, &uds_fd_table[minor].addr,
						sizeof(struct sockaddr_un));
	}

	/* revive peer that was waiting for us to write */
	if (uds_fd_table[peer].suspended) {
		uds_fd_table[peer].ready_to_revive = 1;
		uds_unsuspend(m_source, peer);
	}

	/* see if peer is blocked on select()*/
	if (uds_fd_table[peer].selecting == 1 && fs_m_out.RES_NBYTES > 0) {

		/* if the peer wants to know about data ready to read
		 * and it doesn't know about it already, then let the peer
		 * know we have data for it.
		 */
		if ((uds_fd_table[peer].sel_ops_in & SEL_RD) &&
				!(uds_fd_table[peer].sel_ops_out & SEL_RD)) {

			/* a read on peer is possible now */
			uds_fd_table[peer].sel_ops_out |= SEL_RD;
			uds_fd_table[peer].status_updated = 1;
			uds_unsuspend(m_source, peer);
		}
	}

	return fs_m_out.RES_NBYTES; /* return number of bytes written */
}

int uds_read(message *dev_m_in, message *dev_m_out)
{
	int bytes;
	int minor;

#if DEBUG == 1
	static int call_count = 0;
	printf("(uds) [%d] uds_read() call_count=%d\n", uds_minor(dev_m_in),
							++call_count);
	printf("Endpoint: 0x%x | Position 0x%x\n", dev_m_in->USER_ENDPT,
							dev_m_in->POSITION);
#endif

	minor = uds_minor(dev_m_in);

	if (uds_fd_table[minor].state != UDS_INUSE) {

		/* attempted to close a socket that hasn't been opened --
		 * something is very wrong :(
		 */
		uds_set_reply(dev_m_out, DEV_REVIVE, dev_m_in->USER_ENDPT,
			      (cp_grant_id_t) dev_m_in->IO_GRANT, EINVAL);

		return EINVAL;
	}

	/* track the system call we are performing in case it gets cancelled */
	uds_fd_table[minor].call_nr = dev_m_in->m_type;
	uds_fd_table[minor].ioctl = 0;
	uds_fd_table[minor].syscall_done = 0;

	/* Update the process endpoint. */
	uds_fd_table[minor].endpoint = dev_m_in->USER_ENDPT;

	/* setup select(2) framework */
	uds_fd_table[minor].selecting = 0;

	/* save I/O Grant info */
	uds_fd_table[minor].io_gr = (cp_grant_id_t) dev_m_in->IO_GRANT;
	uds_fd_table[minor].io_gr_size = dev_m_in->COUNT;

	bytes = uds_perform_read(minor, dev_m_in->m_source,
					uds_fd_table[minor].io_gr_size, 0);

	uds_set_reply(dev_m_out, DEV_REVIVE, uds_fd_table[minor].endpoint,
		      uds_fd_table[minor].io_gr, bytes);

	return bytes;
}

int uds_write(message *dev_m_in, message *dev_m_out)
{
	int bytes;
	int minor;

#if DEBUG == 1
	static int call_count = 0;
	printf("(uds) [%d] uds_write() call_count=%d\n", uds_minor(dev_m_in),
							++call_count);
	printf("Endpoint: 0x%x | Position 0x%x\n", dev_m_in->USER_ENDPT,
							dev_m_in->POSITION);
#endif

	minor = uds_minor(dev_m_in);

	if (uds_fd_table[minor].state != UDS_INUSE) {

		/* attempted to write to a socket that hasn't been opened --
		 * something is very wrong :(
		 */
		uds_set_reply(dev_m_out, DEV_REVIVE, dev_m_in->USER_ENDPT,
			      (cp_grant_id_t) dev_m_in->IO_GRANT, EINVAL);

		return EINVAL;
	}

	/* track the system call we are performing in case it gets cancelled */
	uds_fd_table[minor].call_nr = dev_m_in->m_type;
	uds_fd_table[minor].ioctl = 0;
	uds_fd_table[minor].syscall_done = 0;

	/* Update the process endpoint. */
	uds_fd_table[minor].endpoint = dev_m_in->USER_ENDPT;

	/* setup select(2) framework */
	uds_fd_table[minor].selecting = 0;

	/* save I/O Grant info */
	uds_fd_table[minor].io_gr = (cp_grant_id_t) dev_m_in->IO_GRANT;
	uds_fd_table[minor].io_gr_size = dev_m_in->COUNT;

	bytes = uds_perform_write(minor, dev_m_in->m_source,
					uds_fd_table[minor].io_gr_size, 0);

	uds_set_reply(dev_m_out, DEV_REVIVE, uds_fd_table[minor].endpoint,
		      uds_fd_table[minor].io_gr, bytes);

	return bytes;
}

int uds_ioctl(message *dev_m_in, message *dev_m_out)
{
	int rc, minor;

#if DEBUG == 1
	static int call_count = 0;
	printf("(uds) [%d] uds_ioctl() call_count=%d\n", uds_minor(dev_m_in),
							++call_count);
	printf("Endpoint: 0x%x | Position 0x%x\n", dev_m_in->USER_ENDPT,
							dev_m_in->POSITION);
#endif

	minor = uds_minor(dev_m_in);

	if (uds_fd_table[minor].state != UDS_INUSE) {

		/* attempted to close a socket that hasn't been opened --
		 * something is very wrong :(
		 */
		uds_set_reply(dev_m_out, DEV_REVIVE, dev_m_in->USER_ENDPT,
			      (cp_grant_id_t) dev_m_in->IO_GRANT, EINVAL);

		return EINVAL;
	}

	/* track the system call we are performing in case it gets cancelled */
	uds_fd_table[minor].call_nr = dev_m_in->m_type;
	uds_fd_table[minor].ioctl = dev_m_in->COUNT;
	uds_fd_table[minor].syscall_done = 0;

	/* setup select(2) framework */
	uds_fd_table[minor].selecting = 0;

	/* update the owner endpoint - yes it's really stored in POSITION */
	uds_fd_table[minor].owner = dev_m_in->POSITION;

	switch (dev_m_in->COUNT) {	/* Handle the ioctl(2) command */

		case NWIOSUDSCONN:

			/* connect to a listening socket -- connect() */
			rc = do_connect(dev_m_in, dev_m_out);

			break;

		case NWIOSUDSACCEPT:

			/* accept an incoming connection -- accept() */
			rc = do_accept(dev_m_in, dev_m_out);

			break;

		case NWIOSUDSBLOG:

			/* set the backlog_size and put the socket into the
			 * listening state -- listen()
			 */
			rc = do_listen(dev_m_in, dev_m_out);

			break;

		case NWIOSUDSTYPE:

			/* set the type for this socket (i.e.
			 * SOCK_STREAM, SOCK_DGRAM, etc) -- socket()
			 */
			rc = do_socket(dev_m_in, dev_m_out);

			break;

		case NWIOSUDSADDR:

			/* set the address for this socket -- bind() */
			rc = do_bind(dev_m_in, dev_m_out);

			break;

		case NWIOGUDSADDR:

			/* get the address for this socket -- getsockname() */
			rc = do_getsockname(dev_m_in, dev_m_out);

			break;

		case NWIOGUDSPADDR:

			/* get the address for the peer -- getpeername() */
			rc = do_getpeername(dev_m_in, dev_m_out);

			break;

		case NWIOSUDSSHUT:

			/* shutdown a socket for reading, writing, or
			 * both -- shutdown()
			 */
			rc = do_shutdown(dev_m_in, dev_m_out);

			break;

		case NWIOSUDSPAIR:

			/* connect two sockets -- socketpair() */
			rc = do_socketpair(dev_m_in, dev_m_out);

			break;

		case NWIOGUDSSOTYPE:

			/* get socket type -- getsockopt(SO_TYPE) */
			rc = do_getsockopt_sotype(dev_m_in, dev_m_out);

			break;

		case NWIOGUDSPEERCRED:

			/* get peer endpoint -- getsockopt(SO_PEERCRED) */
			rc = do_getsockopt_peercred(dev_m_in, dev_m_out);

			break;

		case NWIOSUDSTADDR:

			/* set target address -- sendto() */
			rc = do_sendto(dev_m_in, dev_m_out);

			break;

		case NWIOGUDSFADDR:

			/* get from address -- recvfrom() */
			rc = do_recvfrom(dev_m_in, dev_m_out);

			break;

		case NWIOGUDSSNDBUF:

			/* get the send buffer size -- getsockopt(SO_SNDBUF) */
			rc = do_getsockopt_sndbuf(dev_m_in, dev_m_out);

			break;

		case NWIOSUDSSNDBUF:

			/* set the send buffer size -- setsockopt(SO_SNDBUF) */
			rc = do_setsockopt_sndbuf(dev_m_in, dev_m_out);

			break;

		case NWIOGUDSRCVBUF:

			/* get the send buffer size -- getsockopt(SO_SNDBUF) */
			rc = do_getsockopt_rcvbuf(dev_m_in, dev_m_out);

			break;

		case NWIOSUDSRCVBUF:

			/* set the send buffer size -- setsockopt(SO_SNDBUF) */
			rc = do_setsockopt_rcvbuf(dev_m_in, dev_m_out);

			break;

		case NWIOSUDSCTRL:

			/* set the control data -- sendmsg() */
			rc = do_sendmsg(dev_m_in, dev_m_out);

			break;

		case NWIOGUDSCTRL:

			/* set the control data -- recvmsg() */
			rc = do_recvmsg(dev_m_in, dev_m_out);

			break;

		default:

			/* the IOCTL command is not valid for /dev/uds --
			 * this happens a lot and is normal. a lot of
			 * libc functions determine the socket type with
			 * IOCTLs. Any not for us simply get a EBADIOCTL
			 * response.
			 */

			rc = EBADIOCTL;
	}

	if (rc != SUSPEND)
		uds_fd_table[minor].syscall_done = 1;

	uds_set_reply(dev_m_out, DEV_REVIVE, dev_m_in->USER_ENDPT,
		      (cp_grant_id_t) dev_m_in->IO_GRANT, rc);

	return rc;
}

int uds_unsuspend(endpoint_t m_source, int minor)
{
	int r = OK, bytes;
	message m_out;
	uds_fd_t *fdp;

	fdp = &uds_fd_table[minor];

	if (fdp->status_updated == 1) {

		/* clear the status_updated flag */
		fdp->status_updated = 0;
		fdp->selecting = 0;

		/* prepare the response */
		uds_sel_reply(&m_out, DEV_SEL_REPL2, minor, fdp->sel_ops_out);
	} else if (fdp->ready_to_revive == 1) {

		/* clear the ready to revive flag */
		fdp->ready_to_revive = 0;

		switch (fdp->suspended) {

			case UDS_SUSPENDED_READ:

				bytes = uds_perform_read(minor, m_source,
							 fdp->io_gr_size, 0);

				if (bytes == SUSPEND) {
					r = SUSPEND;
					break;
				}

				fdp->suspended = UDS_NOT_SUSPENDED;

				uds_set_reply(&m_out, DEV_REVIVE, fdp->endpoint,
					      fdp->io_gr, bytes);

				break;

			case UDS_SUSPENDED_WRITE:

				bytes = uds_perform_write(minor, m_source,
							  fdp->io_gr_size, 0);

				if (bytes == SUSPEND) {
					r = SUSPEND;
					break;
				}

				fdp->suspended = UDS_NOT_SUSPENDED;

				uds_set_reply(&m_out, DEV_REVIVE, fdp->endpoint,
					      fdp->io_gr, bytes);

				break;

			case UDS_SUSPENDED_CONNECT:
			case UDS_SUSPENDED_ACCEPT:

				/* In both cases, the process
				 * that send the notify()
				 * already performed the connection.
				 * The only thing to do here is
				 * unblock.
				 */

				fdp->suspended = UDS_NOT_SUSPENDED;

				uds_set_reply(&m_out, DEV_REVIVE, fdp->endpoint,
					      fdp->io_gr, OK);

				break;

			default:
				return(OK);
		}

	}

	if (r == OK) reply(m_source, &m_out);
	return(r);
}

int uds_cancel(message *dev_m_in, message *dev_m_out)
{
	int i, j;
	int minor;
	/* XXX: should become a noop? */
#if DEBUG == 1
	static int call_count = 0;
	printf("(uds) [%d] uds_cancel() call_count=%d\n", uds_minor(dev_m_in),
							++call_count);
	printf("Endpoint: 0x%x\n", dev_m_in->USER_ENDPT);
#endif

	minor = uds_minor(dev_m_in);

	if (uds_fd_table[minor].state != UDS_INUSE) {
		/* attempted to cancel an unknown request - this happens */
		return SUSPEND;
	}

	/* Update the process endpoint. */
	uds_fd_table[minor].endpoint = dev_m_in->USER_ENDPT;

	/* setup select(2) framework */
	uds_fd_table[minor].selecting = 0;

	/* the system call was cancelled, so if the socket was suspended
	 * (which is likely the case), then it is not suspended anymore.
	 */
	uds_fd_table[minor].suspended = UDS_NOT_SUSPENDED;

	/* If there is a system call and it isn't complete, roll back */
	if (uds_fd_table[minor].call_nr && !uds_fd_table[minor].syscall_done) {


		if  (uds_fd_table[minor].call_nr == DEV_IOCTL_S) {

			switch (uds_fd_table[minor].ioctl) {

				case NWIOSUDSACCEPT:	/* accept() */

					/* partial accept() only changes
					 * uds_fd_table[minorparent].child
					 */

					for (i = 0; i < NR_FDS; i++) {
						if (uds_fd_table[i].child ==
							minor) {

						uds_fd_table[i].child = -1;

						}
					}

					break;

				case NWIOSUDSCONN:	/* connect() */

					/* partial connect() sets addr
					 * and adds minor to server backlog
					 */

					for (i = 0; i < NR_FDS; i++) {

						/* find a socket that is in
						 * use.
						 */
						if (uds_fd_table[i].state ==
							UDS_INUSE) {

							/* see if minor is in
							 * the backlog
							 */
			for (j = 0; j < uds_fd_table[i].backlog_size; j++) {

				if (uds_fd_table[i].backlog[j] == minor) {

					/* remove from backlog */
					uds_fd_table[i].backlog[j] = -1;
				}
			}

						}
					}

					/* clear the address */
					memset(&(uds_fd_table[minor].addr),
						'\0',
						sizeof(struct sockaddr_un));

					break;

				case NWIOSUDSTADDR:	/* sendto() */
				case NWIOSUDSADDR:	/* bind() */
				case NWIOGUDSADDR:	/* getsockname() */
				case NWIOGUDSPADDR:	/* getpeername() */
				case NWIOSUDSTYPE:	/* socket() */
				case NWIOSUDSBLOG:	/* listen() */
				case NWIOSUDSSHUT:	/* shutdown() */
				case NWIOSUDSPAIR:	/* socketpair() */
				case NWIOGUDSSOTYPE:	/* SO_TYPE */
				case NWIOGUDSPEERCRED:	/* SO_PEERCRED */
				default:
					/* these are atomic, never suspend,
					 * and can't be cancelled once called
					 */
					break;
			}

		}

		/* DEV_READ_S or DEV_WRITE_S don't need to do anything
		 * when cancelled. DEV_OPEN, DEV_REOPEN, DEV_SELECT,
		 * DEV_CLOSE are atomic, never suspend, and can't
		 * be cancelled once called.
		 */

		uds_fd_table[minor].syscall_done = 1;
	}

	uds_set_reply(dev_m_out, DEV_REVIVE, dev_m_in->USER_ENDPT,
			(cp_grant_id_t) dev_m_in->IO_GRANT, EINTR);

	return EINTR;
}
