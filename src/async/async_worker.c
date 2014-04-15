/*-
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __async_flush_wait --
 *	Wait for the final worker to finish flushing.
 *	Assumes it is called with spinlock held and returns it locked.
 */
static int
__async_flush_wait(WT_SESSION_IMPL *session, WT_ASYNC *async, int *locked)
{
	WT_DECL_RET;

	/*
	 * We change the caller's locked setting so that if we return an
	 * error from this function the caller can properly unlock or not
	 * as needed.
	 */
	while (FLD_ISSET(async->opsq_flush, WT_ASYNC_FLUSHING)) {
		__wt_spin_unlock(session, &async->opsq_lock);
		*locked= 0;
		WT_ERR_TIMEDOUT_OK(
		    __wt_cond_wait(session, async->flush_cond, 10000));
		__wt_spin_lock(session, &async->opsq_lock);
		*locked= 1;
	}
err:	return (ret);
}

/*
 * __async_worker_cursor --
 *	Return a cursor for the worker thread to use for its op.
 *	The worker thread caches cursors.  So first search for one
 *	with the same config/uri signature.  Otherwise open a new
 *	cursor and cache it.
 */
static int
__async_worker_cursor(WT_SESSION_IMPL *session, WT_ASYNC_OP_IMPL *op,
    WT_ASYNC_WORKER_STATE *worker, WT_CURSOR **cursorp)
{
	WT_ASYNC_CURSOR *ac;
	WT_CURSOR *c;
	WT_DECL_RET;
	WT_SESSION *wt_session;

	wt_session = (WT_SESSION *)session;
	*cursorp = NULL;
	WT_ASSERT(session, op->format != NULL);
	STAILQ_FOREACH(ac, &worker->cursorqh, q) {
		if (op->format->cfg_hash == ac->cfg_hash &&
		    op->format->uri_hash == ac->uri_hash) {
			/*
			 * If one of our cached cursors has a matching
			 * signature, use it and we're done.
			 */
			*cursorp = ac->c;
			return (0);
		}
	}
	/*
	 * We didn't find one in our cache.  Open one and cache it.
	 * Insert it at the head expecting LRU usage.
	 */
	WT_RET(__wt_calloc_def(session, 1, &ac));
	WT_ERR(wt_session->open_cursor(
	    wt_session, op->format->uri, NULL, op->format->config, &c));
	ac->cfg_hash = op->format->cfg_hash;
	ac->uri_hash = op->format->uri_hash;
	ac->c = c;
	STAILQ_INSERT_HEAD(&worker->cursorqh, ac, q);
	worker->num_cursors++;
	*cursorp = c;
	return (0);

err:	__wt_free(session, ac);
	return (ret);
}

/*
 * __async_worker_execop --
 *	A worker thread executes an individual op with a cursor.
 */
static int
__async_worker_execop(WT_SESSION_IMPL *session, WT_ASYNC_OP_IMPL *op,
    WT_CURSOR *cursor)
{
	WT_ASYNC_OP *asyncop;
	WT_DECL_RET;
	WT_ITEM val;

	asyncop = (WT_ASYNC_OP *)op;
	/*
	 * Set the key of our local cursor from the async op handle.
	 * If needed, also set the value.
	 */
	__wt_cursor_set_raw_key(cursor, &asyncop->c.key);
	if (op->optype != WT_AOP_SEARCH && op->optype != WT_AOP_REMOVE)
		__wt_cursor_set_raw_value(cursor, &asyncop->c.value);
	switch (op->optype) {
		case WT_AOP_INSERT:
		case WT_AOP_UPDATE:
			WT_ERR(cursor->insert(cursor));
			break;
		case WT_AOP_REMOVE:
			WT_ERR(cursor->remove(cursor));
			break;
		case WT_AOP_SEARCH:
			WT_ERR(cursor->search(cursor));
			/*
			 * Get the value from the cursor and put it into
			 * the op for op->get_value.
			 */
			__wt_cursor_get_raw_value(cursor, &val);
			__wt_cursor_set_raw_value(&asyncop->c, &val);
			break;
		default:
			WT_ERR_MSG(session, EINVAL, "Unknown async optype %d\n",
			    op->optype);
	}
err:
	return (ret);
}

/*
 * __async_worker_op --
 *	A worker thread handles an individual op.
 */
static int
__async_worker_op(WT_SESSION_IMPL *session, WT_ASYNC_OP_IMPL *op,
    WT_ASYNC_WORKER_STATE *worker)
{
	WT_ASYNC_OP *asyncop;
	WT_CURSOR *cursor;
	WT_DECL_RET;
	int cb_ret;

	asyncop = (WT_ASYNC_OP *)op;

	ret = 0;
	cb_ret = 0;

	WT_RET(__wt_txn_begin(session, NULL));
	WT_ASSERT(session, op->state == WT_ASYNCOP_WORKING);
	WT_RET(__async_worker_cursor(session, op, worker, &cursor));
	/*
	 * Perform op and invoke the callback.
	 */
	ret = __async_worker_execop(session, op, cursor);
	if (op->cb != NULL && op->cb->notify != NULL)
		cb_ret = op->cb->notify(op->cb, asyncop, ret, 0);

	/*
	 * If the operation succeeded and the user callback returned
	 * zero then commit.  Otherwise rollback.
	 */
	if ((ret == 0 || ret == WT_NOTFOUND) && cb_ret == 0)
		WT_TRET(__wt_txn_commit(session, NULL));
	else
		WT_TRET(__wt_txn_rollback(session, NULL));
	/*
	 * After the callback returns, and the transaction resolved release
	 * the op back to the free pool and reset our cached cursor.
	 */
	ret = 0;
	op->state = WT_ASYNCOP_FREE;
	F_CLR(&asyncop->c, WT_CURSTD_KEY_SET | WT_CURSTD_VALUE_SET);
	cursor->reset(cursor);
	return (ret);
}

/*
 * __async_worker --
 *	The async worker threads.
 */
void *
__wt_async_worker(void *arg)
{
	WT_ASYNC *async;
	WT_ASYNC_CURSOR *ac, *acnext;
	WT_ASYNC_OP_IMPL *op;
	WT_ASYNC_WORKER_STATE worker;
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;
	int locked;

	session = arg;
	conn = S2C(session);
	async = conn->async;

	locked = 0;
	worker.num_cursors = 0;
	STAILQ_INIT(&worker.cursorqh);
	while (F_ISSET(conn, WT_CONN_SERVER_RUN)) {
		__wt_spin_lock(session, &async->opsq_lock);
		locked = 1;
		if (FLD_ISSET(async->opsq_flush, WT_ASYNC_FLUSHING)) {
			/*
			 * Worker flushing going on.  Last worker to the party
			 * needs to clear the FLUSHING flag and signal the cond.
			 * If FLUSHING is going on, we do not take anything off
			 * the queue.
			 */
			if (++async->flush_count == conn->async_workers) {
				/*
				 * We're last.  All workers accounted for so
				 * signal the condition and clear the FLUSHING
				 * flag to release the other worker threads.
				 * Set the FLUSH_COMPLETE flag so that the
				 * caller can return to the application.
				 */
				FLD_SET(async->opsq_flush,
				    WT_ASYNC_FLUSH_COMPLETE);
				FLD_CLR(async->opsq_flush, WT_ASYNC_FLUSHING);
				__wt_spin_unlock(session, &async->opsq_lock);
				locked = 0;
				WT_ERR(__wt_cond_signal(session,
				    async->flush_cond));
				__wt_spin_lock(session, &async->opsq_lock);
				locked = 1;
			} else {
				/*
				 * We need to wait for the last worker to
				 * signal the condition.
				 */
				WT_ERR(__async_flush_wait(
				    session, async, &locked));
			}
		}
		/*
		 * Get next op.  We get here with the opsq lock held.
		 * Remove from the head of the queue.
		 */
		op = STAILQ_FIRST(&async->opqh);
		if (op == NULL) {
			__wt_spin_unlock(session, &async->opsq_lock);
			locked = 0;
			goto wait_for_work;
		}

		/*
		 * There is work to do.
		 */
		STAILQ_REMOVE_HEAD(&async->opqh, q);
		WT_ASSERT(session, async->cur_queue > 0);
		--async->cur_queue;
		WT_ASSERT(session, op->state == WT_ASYNCOP_ENQUEUED);
		op->state = WT_ASYNCOP_WORKING;
		if (op == &async->flush_op) {
			WT_ASSERT(session, FLD_ISSET(async->opsq_flush,
			    WT_ASYNC_FLUSH_IN_PROGRESS));
			/*
			 * We're the worker to take the flush op off the queue.
			 * Set the flushing flag and set count to 1.
			 */
			FLD_SET(async->opsq_flush, WT_ASYNC_FLUSHING);
			async->flush_count = 1;
			WT_ERR(__async_flush_wait(session, async, &locked));
		}
		/*
		 * Release the lock before performing the op.
		 */
		__wt_spin_unlock(session, &async->opsq_lock);
		locked = 0;
		if (op != &async->flush_op)
			(void)__async_worker_op(session, op, &worker);

wait_for_work:
		WT_ASSERT(session, locked == 0);
		/* Wait until the next event. */
#if 0
		WT_ERR_TIMEDOUT_OK(
		    __wt_cond_wait(session, async->ops_cond, 100000));
#else
		__wt_yield();
#endif
	}

	if (0) {
err:		__wt_err(session, ret, "async worker error");
		if (locked)
			__wt_spin_unlock(session, &async->opsq_lock);
	}
	/*
	 * Worker thread cleanup, close our cached cursors and
	 * free all the WT_ASYNC_CURSOR structures.
	 */
	ac = STAILQ_FIRST(&worker.cursorqh);
	while (ac != NULL) {
		acnext = STAILQ_NEXT(ac, q);
		WT_TRET(ac->c->close(ac->c));
		__wt_free(session, ac);
		ac = acnext;
	}
	return (NULL);
}
