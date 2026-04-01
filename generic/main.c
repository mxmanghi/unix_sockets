#if defined(HAVE_CONFIG_H)
#include <config.h>
#endif

#include "tclstuff.h"
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <string.h>
#include <stddef.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

/* Tcl_Size compat for Tcl 8 builds without tclconfig support */
#ifndef TCL_SIZE_MAX
#ifndef Tcl_Size
typedef int Tcl_Size;
#endif
#endif

#define MAX_UDS_PATH	(sizeof(((struct sockaddr_un*)0)->sun_path) - 1)


typedef struct uds_state {
	Tcl_Channel	channel;
	int			fd;
	Tcl_Obj*	path;

	// Set if this is an accept handler (listening socket), so that we know
	// whether to unlink the socket file on close, and whether to unlink from
	// the interp_cx accept handlers list on close. If this is not an accept
	// handler, these fields will be NULL and ignored.
	Tcl_Interp*	interp;
	Tcl_Obj*	accept_handler;
	struct uds_state*	next;
	struct uds_state*	prev;
} uds_state;

struct interp_cx {
	// doubly-linked list of accept callbacks (uds_state instances) active in this interp
	struct uds_state*	accept_handlers_head;
};

static int close2Proc(ClientData cdata, Tcl_Interp* interp, int flags) //<<<
{
	uds_state*	con = cdata;
	(void)interp;

	if (flags & (TCL_CLOSE_READ | TCL_CLOSE_WRITE))
		return EINVAL;

	Tcl_DeleteFileHandler(con->fd);

	if (con->accept_handler != NULL) {
		unlink(Tcl_GetString(con->path));

		// Unlink from the interp_cx accept handlers list.  Use con->interp
		// (the interp that owns the listen socket), not the interp parameter
		// (which is the interp closing the channel — could be NULL or different).
		// If con->interp is NULL, the interp was already deleted and
		// delete_interp_cx has freed the list head, so skip unlinking.
		if (con->interp) {
			if (con->prev) {
				con->prev->next = con->next;
			} else {
				struct interp_cx* cx = Tcl_GetAssocData(con->interp, PACKAGE_NAME, NULL);
				cx->accept_handlers_head = con->next;
			}
			if (con->next)
				con->next->prev = con->prev;
		}
	}

	replace_tclobj(&con->accept_handler,	NULL);
	replace_tclobj(&con->path,				NULL);

	con->interp = NULL;

	close(con->fd);
	ckfree(con); con = NULL;

	return 0;
}

//>>>
static int inputProc(ClientData cdata, char* buf, int bufSize, int *errorCodePtr) //<<<
{
	uds_state*	con = cdata;
	int			got;

	got = read(con->fd, buf, bufSize);

	if (got == -1)
		*errorCodePtr = Tcl_GetErrno();

	return got;
}

//>>>
static int outputProc(ClientData cdata, const char* buf, int toWrite, int* errorCodePtr) //<<<
{
	uds_state*	con = cdata;
	int			wrote;

	wrote = send(con->fd, buf, (size_t)toWrite, 0);
	if (wrote == -1)
		*errorCodePtr = Tcl_GetErrno();

	return wrote;
}

//>>>
static int blockModeProc(ClientData cdata, int mode) //<<<
{
	uds_state*	con = cdata;
	int			flags;

	flags = fcntl(con->fd, F_GETFL);
	if (flags == -1)
		return Tcl_GetErrno();

#ifdef O_NDELAY
	flags &= ~O_NDELAY;
#endif

	if (mode == TCL_MODE_BLOCKING)
		flags &= ~O_NONBLOCK;
	else
		flags |= O_NONBLOCK;

	if (fcntl(con->fd, F_SETFL, flags) == -1)
		return Tcl_GetErrno();

	return 0;
}

//>>>
static void watchProc(ClientData cdata, int mask) //<<<
{
	uds_state*	con = cdata;

	if (mask)
		Tcl_CreateFileHandler(con->fd, mask, (Tcl_FileProc*)Tcl_NotifyChannel, con->channel);
	else
		Tcl_DeleteFileHandler(con->fd);
}

//>>>
static int getHandleProc(ClientData cdata, int direction, ClientData* handlePtr) //<<<
{
	uds_state*	con = cdata;
	(void)direction;

	ptrdiff_t	cd = con->fd;
	*handlePtr = (ClientData)cd;

	return TCL_OK;
}

//>>>

static Tcl_ChannelType unix_socket_channel_type = {
	.typeName		= "unix_socket",
#if TCL_MAJOR_VERSION > 8
	.version		= TCL_CHANNEL_VERSION_5,
#else
	.version		= TCL_CHANNEL_VERSION_2,
#endif
	.inputProc		= inputProc,
	.outputProc		= outputProc,
	.watchProc		= watchProc,
	.getHandleProc	= getHandleProc,
	.close2Proc		= close2Proc,
	.blockModeProc	= blockModeProc,
};


static void accept_dispatcher(ClientData cdata, int mask) //<<<
{
	int					code = TCL_OK;
	uds_state*			state = cdata;
	Tcl_Interp*			interp = state->interp;	// local alias for tclstuff macros
	struct sockaddr_un	client_address;
	int					client_sockfd;
	socklen_t			client_len;
	Tcl_Obj*			channel_name = NULL;
	Tcl_Obj*			handler = NULL;
	Tcl_Channel			channel = NULL;
	uds_state*			con = NULL;
	(void)mask;

	if (!interp) {
		// Interp was deleted: no useful way to invoke the accept handler.
		// Drain the pending connection so we don't busy-loop, and remove the
		// file handler since no further accepts can be dispatched.
		Tcl_DeleteFileHandler(state->fd);
		client_len = sizeof(client_address);
		client_sockfd = accept(state->fd, (struct sockaddr*)&client_address, &client_len);
		if (client_sockfd != -1) close(client_sockfd);
		return;
	}

	Tcl_Preserve(interp);

	client_len = sizeof(client_address);
	if (-1 == (client_sockfd = accept(state->fd, (struct sockaddr*)&client_address, &client_len)))
		THROW_POSIX_LABEL(finally, code, "accept failed");

	// Stop this fd from being inherited by children
	(void)fcntl(client_sockfd, F_SETFD, FD_CLOEXEC);

	con = (uds_state*)ckalloc(sizeof(uds_state));
	*con = (uds_state){
		.fd		= client_sockfd,
	};

	replace_tclobj(&channel_name, Tcl_ObjPrintf("unix_socket%d", client_sockfd));
	channel = Tcl_CreateChannel(&unix_socket_channel_type, Tcl_GetString(channel_name), con, (TCL_READABLE | TCL_WRITABLE));
	Tcl_RegisterChannel(NULL, channel);	// Hold a ref for our channel stack var
	con->channel = channel;
	Tcl_RegisterChannel(interp, channel);
	con = NULL; // channel now owns this memory

	// Call the handler with the channel name as argument
	replace_tclobj(&handler, Tcl_DuplicateObj(state->accept_handler));
	TEST_OK_LABEL(finally, code, Tcl_ListObjAppendElement(interp, handler, channel_name));
	TEST_OK_LABEL(finally, code, Tcl_EvalObjEx(interp, handler, TCL_EVAL_GLOBAL));

finally:
	replace_tclobj(&channel_name,	NULL);
	replace_tclobj(&handler,		NULL);

	if (con) {
		close(con->fd);
		ckfree(con); con = NULL;
	}
	if (code != TCL_OK) {
		Tcl_BackgroundError(interp);
		if (channel) Tcl_UnregisterChannel(interp, channel);
	}
	if (channel) Tcl_UnregisterChannel(NULL, channel);	// Release protection ref

	Tcl_Release(interp);
}

//>>>
static OBJCMD(glue_listen) //<<<
{
	int					code = TCL_OK;
	int					server_sockfd = -1;
	struct sockaddr_un	server_address;
	const char*			path;
	Tcl_Size			path_len;
	uds_state*			state;
	Tcl_Obj*			channel_name = NULL;
	Tcl_Channel			channel;
	struct interp_cx*	cx = Tcl_GetAssocData(interp, PACKAGE_NAME, NULL);
	(void)cdata;

	enum {A_cmd, A_PATH, A_HANDLER, A_objc};
	CHECK_ARGS_LABEL(finally, code, "path accept_handler");

	path = Tcl_GetStringFromObj(objv[A_PATH], &path_len);
	if (path_len > (Tcl_Size)MAX_UDS_PATH)
		THROW_ERROR_LABEL(finally, code, "path is too long");

	if (-1 == (server_sockfd = socket(AF_UNIX, SOCK_STREAM, 0)))
		THROW_POSIX_LABEL(finally, code, "socket");

	(void)fcntl(server_sockfd, F_SETFD, FD_CLOEXEC);

	memset(&server_address, 0, sizeof(server_address));
	server_address.sun_family = AF_UNIX;
	memcpy(server_address.sun_path, path, path_len);

	unlink(path);
	if (-1 == bind(server_sockfd, (struct sockaddr*)&server_address, sizeof(server_address)))
		THROW_POSIX_LABEL(finally, code, "bind");

	if (-1 == listen(server_sockfd, 100))
		THROW_POSIX_LABEL(finally, code, "listen");

	replace_tclobj(&channel_name, Tcl_ObjPrintf("unix_socket%d", server_sockfd));

	state = (uds_state*)ckalloc(sizeof(uds_state));
	*state = (uds_state){
		.fd			= server_sockfd,

		// We store a ref to this interp, so hook this into the linked list to
		// clear .interp when the interp is being deleted
		.interp		= interp,
		.next		= cx->accept_handlers_head,
	};
	if (cx->accept_handlers_head)
		cx->accept_handlers_head->prev = state;
	cx->accept_handlers_head = state;
	server_sockfd = -1; // state now owns this fd

	channel = Tcl_CreateChannel(&unix_socket_channel_type, Tcl_GetString(channel_name), state, 0);
	state->channel = channel;
	replace_tclobj(&state->accept_handler,	objv[A_HANDLER]);
	replace_tclobj(&state->path,			objv[A_PATH]);

	Tcl_RegisterChannel(interp, channel);

	Tcl_CreateFileHandler(state->fd, TCL_READABLE, accept_dispatcher, state);

	Tcl_SetObjResult(interp, channel_name);

finally:
	replace_tclobj(&channel_name, NULL);
	if (server_sockfd != -1) {
		close(server_sockfd);
		server_sockfd = -1;
	}
	return code;
}

//>>>
static OBJCMD(glue_connect) //<<<
{
	int					code = TCL_OK;
	int					fd = -1;
	Tcl_Obj*			channel_name = NULL;
	const char*			path;
	Tcl_Size			path_len;
	struct sockaddr_un	address;
	uds_state*			con;
	(void)cdata;

	enum {A_cmd, A_PATH, A_objc};
	CHECK_ARGS_LABEL(finally, code, "path");

	path = Tcl_GetStringFromObj(objv[A_PATH], &path_len);
	if (path_len > (Tcl_Size)MAX_UDS_PATH)
		THROW_ERROR_LABEL(finally, code, "path is too long");

	if (-1 == (fd = socket(AF_UNIX, SOCK_STREAM, 0)))
		THROW_POSIX_LABEL(finally, code, "socket");

	// Stop this fd from being inherited by children
	(void)fcntl(fd, F_SETFD, FD_CLOEXEC);

	memset(&address, 0, sizeof(address));
	address.sun_family = AF_UNIX;
	memcpy(address.sun_path, path, path_len);

	if (-1 == connect(fd, (struct sockaddr*)&address, sizeof(address)))
		THROW_POSIX_LABEL(finally, code, "connect");

	replace_tclobj(&channel_name, Tcl_ObjPrintf("unix_socket%d", fd));

	con = (uds_state*)ckalloc(sizeof(uds_state));
	*con = (uds_state){
		.fd		= fd,
	};
	fd = -1; // con now owns this fd

	con->channel = Tcl_CreateChannel(&unix_socket_channel_type, Tcl_GetString(channel_name), con, (TCL_READABLE | TCL_WRITABLE));
	replace_tclobj(&con->path, objv[A_PATH]);

	Tcl_RegisterChannel(interp, con->channel);

	Tcl_SetObjResult(interp, channel_name);

finally:
	replace_tclobj(&channel_name, NULL);
	if (fd != -1) {
		close(fd);
		fd = -1;
	}
	return code;
}

//>>>
void delete_interp_cx(void* cdata, Tcl_Interp* interp) //<<<
{
	struct interp_cx*	cx = cdata;
	uds_state*			cur = cx->accept_handlers_head;
	(void)interp;

	// NULL the interp pointer in all uds_state instances associated with this
	// interp, so that if they are triggered after the interp is deleted, they
	// won't try to use a dangling pointer to the interp. The accept_dispatcher
	// will check for a NULL interp and skip calling the handler if it's NULL.
	while (cur) {
		cur->interp = NULL;
		cur = cur->next;
	}

	ckfree(cdata);
	cdata = NULL;
}

//>>>

int Unix_sockets_Init(Tcl_Interp* interp) //<<<
{
	if (Tcl_InitStubs(interp, TCL_VERSION, 0) == NULL)
		return TCL_ERROR;

	struct interp_cx* cx = (struct interp_cx*)ckalloc(sizeof(struct interp_cx));
	*cx = (struct interp_cx){
		.accept_handlers_head = NULL,
	};
	Tcl_SetAssocData(interp, PACKAGE_NAME, delete_interp_cx, cx);

	NEW_CMD("::" PACKAGE_NAME "::listen",	glue_listen);
	NEW_CMD("::" PACKAGE_NAME "::connect",	glue_connect);

	TEST_OK(Tcl_PkgProvide(interp, PACKAGE_NAME, PACKAGE_VERSION));

	return TCL_OK;
}

//>>>

// vim: foldmethod=marker foldmarker=<<<,>>> ts=4 shiftwidth=4
