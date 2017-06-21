//
// Copyright 2017 Garrett D'Amore <garrett@damore.org>
//
// This software is supplied under the terms of the MIT License, a
// copy of which should be located in the distribution where this
// file was obtained (LICENSE.txt).  A copy of the license may also be
// found online at https://opensource.org/licenses/MIT.
//

#include <stdlib.h>
#include <string.h>

#include "core/nng_impl.h"

// Pair protocol.  The PAIR protocol is a simple 1:1 messaging pattern.
// While a peer is connected to the server, all other peer connection
// attempts are discarded.

typedef struct nni_pair_pipe	nni_pair_pipe;
typedef struct nni_pair_sock	nni_pair_sock;

static void nni_pair_send_cb(void *);
static void nni_pair_recv_cb(void *);
static void nni_pair_getq_cb(void *);
static void nni_pair_putq_cb(void *);
static void nni_pair_pipe_fini(void *);

// An nni_pair_sock is our per-socket protocol private structure.
struct nni_pair_sock {
	nni_sock *	nsock;
	nni_pair_pipe * ppipe;
	nni_msgq *	uwq;
	nni_msgq *	urq;
	int		raw;
};

// An nni_pair_pipe is our per-pipe protocol private structure.  We keep
// one of these even though in theory we'd only have a single underlying
// pipe.  The separate data structure is more like other protocols that do
// manage multiple pipes.
struct nni_pair_pipe {
	nni_pipe *	npipe;
	nni_pair_sock * psock;
	nni_aio		aio_send;
	nni_aio		aio_recv;
	nni_aio		aio_getq;
	nni_aio		aio_putq;
	int		busy;
	int		closed;
};

static int
nni_pair_sock_init(void **sp, nni_sock *nsock)
{
	nni_pair_sock *psock;

	if ((psock = NNI_ALLOC_STRUCT(psock)) == NULL) {
		return (NNG_ENOMEM);
	}
	psock->nsock = nsock;
	psock->ppipe = NULL;
	psock->raw = 0;
	psock->uwq = nni_sock_sendq(nsock);
	psock->urq = nni_sock_recvq(nsock);
	*sp = psock;
	return (0);
}


static void
nni_pair_sock_fini(void *arg)
{
	nni_pair_sock *psock = arg;

	if (psock != NULL) {
		NNI_FREE_STRUCT(psock);
	}
}


static int
nni_pair_pipe_init(void **pp, nni_pipe *npipe, void *psock)
{
	nni_pair_pipe *ppipe;
	int rv;

	if ((ppipe = NNI_ALLOC_STRUCT(ppipe)) == NULL) {
		return (NNG_ENOMEM);
	}

	rv = nni_aio_init(&ppipe->aio_send, nni_pair_send_cb, ppipe);
	if (rv != 0) {
		nni_pair_pipe_fini(ppipe);
		return (rv);
	}
	rv = nni_aio_init(&ppipe->aio_recv, nni_pair_recv_cb, ppipe);
	if (rv != 0) {
		nni_pair_pipe_fini(ppipe);
		return (rv);
	}
	rv = nni_aio_init(&ppipe->aio_getq, nni_pair_getq_cb, ppipe);
	if (rv != 0) {
		nni_pair_pipe_fini(ppipe);
		return (rv);
	}
	rv = nni_aio_init(&ppipe->aio_putq, nni_pair_putq_cb, ppipe);
	if (rv != 0) {
		nni_pair_pipe_fini(ppipe);
		return (rv);
	}
	ppipe->npipe = npipe;
	ppipe->psock = psock;
	*pp = ppipe;
	return (0);
}


static void
nni_pair_pipe_fini(void *arg)
{
	nni_pair_pipe *ppipe = arg;

	NNI_ASSERT(ppipe->busy >= 0);
	nni_aio_fini(&ppipe->aio_send);
	nni_aio_fini(&ppipe->aio_recv);
	nni_aio_fini(&ppipe->aio_putq);
	nni_aio_fini(&ppipe->aio_getq);
	NNI_FREE_STRUCT(ppipe);
}


static int
nni_pair_pipe_start(void *arg)
{
	nni_pair_pipe *ppipe = arg;
	nni_pair_sock *psock = ppipe->psock;

	if (psock->ppipe != NULL) {
		return (NNG_EBUSY);      // Already have a peer, denied.
	}
	psock->ppipe = ppipe;

	// Schedule a getq on the upper, and a read from the pipe.
	// Each of these also sets up another hold on the pipe itself.
	nni_pipe_hold(ppipe->npipe);
	nni_msgq_aio_get(psock->uwq, &ppipe->aio_getq);
	nni_pipe_hold(ppipe->npipe);
	nni_pipe_aio_recv(ppipe->npipe, &ppipe->aio_recv);

	return (0);
}


static void
nni_pair_pipe_stop(void *arg)
{
	nni_pair_pipe *ppipe = arg;
	nni_pair_sock *psock = ppipe->psock;

	nni_msgq_aio_cancel(psock->uwq, &ppipe->aio_getq);
	nni_msgq_aio_cancel(psock->urq, &ppipe->aio_putq);

	if (psock->ppipe == ppipe) {
		psock->ppipe = NULL;
	}
}


static void
nni_pair_recv_cb(void *arg)
{
	nni_pair_pipe *ppipe = arg;
	nni_pair_sock *psock = ppipe->psock;

	if (nni_aio_result(&ppipe->aio_recv) != 0) {
		nni_pipe_close(ppipe->npipe);
		nni_pipe_rele(ppipe->npipe);
		return;
	}

	ppipe->aio_putq.a_msg = ppipe->aio_recv.a_msg;
	ppipe->aio_recv.a_msg = NULL;
	nni_msgq_aio_put(psock->urq, &ppipe->aio_putq);
}


static void
nni_pair_putq_cb(void *arg)
{
	nni_pair_pipe *ppipe = arg;

	if (nni_aio_result(&ppipe->aio_putq) != 0) {
		nni_msg_free(ppipe->aio_putq.a_msg);
		ppipe->aio_putq.a_msg = NULL;
		nni_pipe_close(ppipe->npipe);
		nni_pipe_rele(ppipe->npipe);
		return;
	}
	nni_pipe_aio_recv(ppipe->npipe, &ppipe->aio_recv);
}


static void
nni_pair_getq_cb(void *arg)
{
	nni_pair_pipe *ppipe = arg;
	nni_pair_sock *psock = ppipe->psock;
	nni_msg *msg;

	if (nni_aio_result(&ppipe->aio_getq) != 0) {
		nni_pipe_close(ppipe->npipe);
		nni_pipe_rele(ppipe->npipe);
		return;
	}

	ppipe->aio_send.a_msg = ppipe->aio_getq.a_msg;
	ppipe->aio_getq.a_msg = NULL;
	nni_pipe_aio_send(ppipe->npipe, &ppipe->aio_send);
}


static void
nni_pair_send_cb(void *arg)
{
	nni_pair_pipe *ppipe = arg;
	nni_pair_sock *psock = ppipe->psock;

	if (nni_aio_result(&ppipe->aio_send) != 0) {
		nni_msg_free(ppipe->aio_send.a_msg);
		ppipe->aio_send.a_msg = NULL;
		nni_pipe_close(ppipe->npipe);
		nni_pipe_rele(ppipe->npipe);
		return;
	}

	nni_msgq_aio_get(psock->uwq, &ppipe->aio_getq);
}


static int
nni_pair_sock_setopt(void *arg, int opt, const void *buf, size_t sz)
{
	nni_pair_sock *psock = arg;
	int rv;

	switch (opt) {
	case NNG_OPT_RAW:
		rv = nni_setopt_int(&psock->raw, buf, sz, 0, 1);
		break;
	default:
		rv = NNG_ENOTSUP;
	}
	return (rv);
}


static int
nni_pair_sock_getopt(void *arg, int opt, void *buf, size_t *szp)
{
	nni_pair_sock *psock = arg;
	int rv;

	switch (opt) {
	case NNG_OPT_RAW:
		rv = nni_getopt_int(&psock->raw, buf, szp);
		break;
	default:
		rv = NNG_ENOTSUP;
	}
	return (rv);
}


// This is the global protocol structure -- our linkage to the core.
// This should be the only global non-static symbol in this file.

static nni_proto_pipe_ops nni_pair_pipe_ops = {
	.pipe_init	= nni_pair_pipe_init,
	.pipe_fini	= nni_pair_pipe_fini,
	.pipe_start	= nni_pair_pipe_start,
	.pipe_stop	= nni_pair_pipe_stop,
};

static nni_proto_sock_ops nni_pair_sock_ops = {
	.sock_init	= nni_pair_sock_init,
	.sock_fini	= nni_pair_sock_fini,
	.sock_setopt	= nni_pair_sock_setopt,
	.sock_getopt	= nni_pair_sock_getopt,
};

nni_proto nni_pair_proto = {
	.proto_self	= NNG_PROTO_PAIR,
	.proto_peer	= NNG_PROTO_PAIR,
	.proto_name	= "pair",
	.proto_flags	= NNI_PROTO_FLAG_SNDRCV,
	.proto_sock_ops = &nni_pair_sock_ops,
	.proto_pipe_ops = &nni_pair_pipe_ops,
};
