/*
 * executor.c
 *
 * Common routines related to query execution phase
 * ----
 * Copyright 2011-2023 (C) KaiGai Kohei <kaigai@kaigai.gr.jp>
 * Copyright 2014-2023 (C) PG-Strom Developers Team
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the PostgreSQL License.
 */
#include "pg_strom.h"
#include "cuda_common.h"

/*
 * XpuConnection
 */
struct XpuConnection
{
	dlist_node		chain;	/* link to gpuserv_connection_slots */
	char			devname[32];
	int				dev_index;		/* cuda_dindex or dpu_endpoint_id */
	volatile pgsocket sockfd;
	volatile int	terminated;		/* positive: normal exit
									 * negative: exit by errors */
	ResourceOwner	resowner;
	pthread_t		worker;
	pthread_mutex_t	mutex;
	int				num_running_cmds;
	int				num_ready_cmds;
	dlist_head		ready_cmds_list;	/* ready, but not fetched yet  */
	dlist_head		active_cmds_list;	/* currently in-use */
	kern_errorbuf	errorbuf;
};

/* see xact.c */
extern int				nParallelCurrentXids;
extern TransactionId   *ParallelCurrentXids;

/* static variables */
static dlist_head		xpu_connections_list;

/*
 * Worker thread to receive response messages
 */
static void *
__xpuConnectAllocCommand(void *__priv, size_t sz)
{
	return malloc(sz);
}

static void
__xpuConnectAttachCommand(void *__priv, XpuCommand *xcmd)
{
	XpuConnection *conn = __priv;

	xcmd->priv = conn;
	pthreadMutexLock(&conn->mutex);
	Assert(conn->num_running_cmds > 0);
	conn->num_running_cmds--;
	if (xcmd->tag == XpuCommandTag__Error)
	{
		if (conn->errorbuf.errcode == ERRCODE_STROM_SUCCESS)
		{
			Assert(xcmd->u.error.errcode != ERRCODE_STROM_SUCCESS);
			memcpy(&conn->errorbuf, &xcmd->u.error, sizeof(kern_errorbuf));
		}
		free(xcmd);
	}
	else
	{
		Assert(xcmd->tag == XpuCommandTag__Success ||
			   xcmd->tag == XpuCommandTag__CPUFallback);
		dlist_push_tail(&conn->ready_cmds_list, &xcmd->chain);
		conn->num_ready_cmds++;
	}
	SetLatch(MyLatch);
	pthreadMutexUnlock(&conn->mutex);
}
TEMPLATE_XPU_CONNECT_RECEIVE_COMMANDS(__xpuConnect)

static void *
__xpuConnectSessionWorker(void *__priv) // QQQ 新线程函数
{
	XpuConnection *conn = __priv;

	for (;;)
	{
		struct pollfd pfd;
		int		sockfd;
		int		nevents;

		sockfd = conn->sockfd;
		if (sockfd < 0)
			break;
		pfd.fd = sockfd;
		pfd.events = POLLIN;
		pfd.revents = 0;
		nevents = poll(&pfd, 1, -1);
		if (nevents < 0)
		{
			if (errno == EINTR)
				continue;
			fprintf(stderr, "[%s; %s:%d] failed on poll(2): %m\n",
					conn->devname, __FILE_NAME__, __LINE__);
			break;
		}
		else if (nevents > 0)
		{
			Assert(nevents == 1);
			if (pfd.revents & ~POLLIN)
			{
				pthreadMutexLock(&conn->mutex);
				conn->terminated = 1;
				SetLatch(MyLatch);
				pthreadMutexUnlock(&conn->mutex);
				return NULL;
			}
			else if (pfd.revents & POLLIN)
			{
				if (__xpuConnectReceiveCommands(conn->sockfd,
												conn,
												conn->devname) < 0)
					break;
			}
		}
	}
	pthreadMutexLock(&conn->mutex);
	conn->terminated = -1;
	SetLatch(MyLatch);
	pthreadMutexUnlock(&conn->mutex);

	return NULL;
}

/*
 * xpuClientSendCommand
 */
void
xpuClientSendCommand(XpuConnection *conn, const XpuCommand *xcmd)
{
	int			sockfd = conn->sockfd;
	const char *buf = (const char *)xcmd;
	size_t		len = xcmd->length;
	ssize_t		nbytes;

	pthreadMutexLock(&conn->mutex);
	conn->num_running_cmds++;
	pthreadMutexUnlock(&conn->mutex);

	while (len > 0)
	{
		nbytes = write(sockfd, buf, len);
		if (nbytes > 0)
		{
			buf += nbytes;
			len -= nbytes;
		}
		else if (nbytes == 0)
			elog(ERROR, "unable to send xPU command to the service");
		else if (errno == EINTR)
			CHECK_FOR_INTERRUPTS();
		else
			elog(ERROR, "failed on write(2): %m");
	}
}

/*
 * xpuClientSendCommandIOV
 */
static void
xpuClientSendCommandIOV(XpuConnection *conn, struct iovec *iov, int iovcnt)
{
	int			sockfd = conn->sockfd;
	ssize_t		nbytes;

	Assert(iovcnt > 0);
	pthreadMutexLock(&conn->mutex);
	conn->num_running_cmds++;
	pthreadMutexUnlock(&conn->mutex);

	while (iovcnt > 0)
	{
		nbytes = writev(sockfd, iov, iovcnt);
		if (nbytes > 0)
		{
			do {
				if (iov->iov_len <= nbytes)
				{
					nbytes -= iov->iov_len;
					iov++;
					iovcnt--;
				}
				else
				{
					iov->iov_base = (char *)iov->iov_base + nbytes;
					iov->iov_len -= nbytes;
					break;
				}
			} while (iovcnt > 0 && nbytes > 0);
		}
		else if (nbytes == 0)
			elog(ERROR, "unable to send xPU command to the service");
		else if (errno == EINTR)
			CHECK_FOR_INTERRUPTS();
		else
			elog(ERROR, "failed on writev(2): %m");
	}
}

/*
 * xpuClientPutResponse
 */
void
xpuClientPutResponse(XpuCommand *xcmd)
{
	XpuConnection  *conn = xcmd->priv;
	
	pthreadMutexLock(&conn->mutex);
	dlist_delete(&xcmd->chain);
	pthreadMutexUnlock(&conn->mutex);
	free(xcmd);
}

/*
 * xpuClientCloseSession
 */
void
xpuClientCloseSession(XpuConnection *conn)
{
	XpuCommand *xcmd;
	dlist_node *dnode;

	/* ensure termination of worker thread */
	close(conn->sockfd);
	conn->sockfd = -1;
	pg_memory_barrier();
	pthread_kill(conn->worker, SIGPOLL);
	pthread_join(conn->worker, NULL);

	while (!dlist_is_empty(&conn->ready_cmds_list))
	{
		dnode = dlist_pop_head_node(&conn->ready_cmds_list);
		xcmd = dlist_container(XpuCommand, chain, dnode);
		free(xcmd);
	}
	while (!dlist_is_empty(&conn->active_cmds_list))
	{
		dnode = dlist_pop_head_node(&conn->active_cmds_list);
		xcmd = dlist_container(XpuCommand, chain, dnode);
		free(xcmd);
	}
	dlist_delete(&conn->chain);
	free(conn);
}

/*
 * xpuclientCleanupConnections
 */
static void
xpuclientCleanupConnections(ResourceReleasePhase phase,
							bool isCommit,
							bool isTopLevel,
							void *arg)
{
	dlist_mutable_iter	iter;

	if (phase != RESOURCE_RELEASE_BEFORE_LOCKS)
		return;

	dlist_foreach_modify(iter, &xpu_connections_list)
	{
		XpuConnection  *conn = dlist_container(XpuConnection,
											   chain, iter.cur);
		if (conn->resowner == CurrentResourceOwner)
		{
			if (isCommit)
				elog(LOG, "Bug? GPU connection %d is not closed on ExecEnd",
					 conn->sockfd);
			xpuClientCloseSession(conn);
		}
	}
}

/* ----------------------------------------------------------------
 *
 * Routines to build session-information
 *
 * ----------------------------------------------------------------
 */
static void
__build_session_param_info(pgstromTaskState *pts,
						   kern_session_info *session,
						   StringInfo buf)
{
	pgstromPlanInfo *pp_info = pts->pp_info;
	ExprContext	   *econtext = pts->css.ss.ps.ps_ExprContext;
	ParamListInfo	param_info = econtext->ecxt_param_list_info;
	ListCell	   *lc;

	Assert(param_info != NULL);
	session->nparams = param_info->numParams;
	foreach (lc, pp_info->used_params)
	{
		Param	   *param = lfirst(lc);
		Datum		param_value;
		bool		param_isnull;
		uint32_t	offset;

		Assert(param->paramid >= 0 &&
			   param->paramid < session->nparams);
		if (param->paramkind == PARAM_EXEC)
		{
			/* See ExecEvalParamExec */
			ParamExecData  *prm = &(econtext->ecxt_param_exec_vals[param->paramid]);

			if (prm->execPlan)
			{
				/* Parameter not evaluated yet, so go do it */
				ExecSetParamPlan(prm->execPlan, econtext);
				/* ExecSetParamPlan should have processed this param... */
				Assert(prm->execPlan == NULL);
			}
			param_isnull = prm->isnull;
			param_value  = prm->value;
		}
		else if (param->paramkind == PARAM_EXTERN)
		{
			/* See ExecEvalParamExtern */
			ParamExternData *prm, prmData;

			if (param_info->paramFetch != NULL)
				prm = param_info->paramFetch(param_info,
											 param->paramid,
											 false, &prmData);
			else
				prm = &param_info->params[param->paramid - 1];
			if (!OidIsValid(prm->ptype))
				elog(ERROR, "no value found for parameter %d", param->paramid);
			if (prm->ptype != param->paramtype)
				elog(ERROR, "type of parameter %d (%s) does not match that when preparing the plan (%s)",
					 param->paramid,
					 format_type_be(prm->ptype),
					 format_type_be(param->paramtype));
			param_isnull = prm->isnull;
			param_value  = prm->value;
		}
		else
		{
			elog(ERROR, "Bug? unexpected parameter kind: %d",
				 (int)param->paramkind);
		}

		if (param_isnull)
			offset = 0;
		else
		{
			int16	typlen;
			bool	typbyval;

			get_typlenbyval(param->paramtype, &typlen, &typbyval);
			if (typbyval)
			{
				offset = __appendBinaryStringInfo(buf,
												  (char *)&param_value,
												  typlen);
			}
			else if (typlen > 0)
			{
				offset = __appendBinaryStringInfo(buf,
												  DatumGetPointer(param_value),
												  typlen);
			}
			else if (typlen == -1)
			{
				struct varlena *temp = PG_DETOAST_DATUM(param_value);

				offset = __appendBinaryStringInfo(buf, temp, VARSIZE(temp));
				if (param_value != PointerGetDatum(temp))
					pfree(temp);
			}
			else
			{
				elog(ERROR, "Not a supported data type for kernel parameter: %s",
					 format_type_be(param->paramtype));
			}
		}
		Assert(param->paramid >= 0 && param->paramid < session->nparams);
		session->poffset[param->paramid] = offset;
	}
}

/*
 * __build_session_kvars_defs
 */
static int
__count_session_kvars_defs_subfields(codegen_kvar_defitem *kvdef)
{
	int		count = list_length(kvdef->kv_subfields);
	ListCell *lc;

	foreach (lc, kvdef->kv_subfields)
	{
		codegen_kvar_defitem *__kvdef = lfirst(lc);

		count += __count_session_kvars_defs_subfields(__kvdef);
	}
	return count;
}

static int
__setup_session_kvars_defs_array(kern_varslot_desc *vslot_desc_root,
								 kern_varslot_desc *vslot_desc_base,
								 List *kvars_deflist)
{
	kern_varslot_desc *vs_desc = vslot_desc_base;
	int			nitems = list_length(kvars_deflist);
	int			count;
	ListCell   *lc;

	vslot_desc_base += nitems;
	foreach (lc, kvars_deflist)
	{
		codegen_kvar_defitem *kvdef = lfirst(lc);

		vs_desc->vs_type_code = kvdef->kv_type_code;
		vs_desc->vs_typbyval  = kvdef->kv_typbyval;
        vs_desc->vs_typalign  = kvdef->kv_typalign;
        vs_desc->vs_typlen    = kvdef->kv_typlen;
		vs_desc->vs_typmod    = exprTypmod((Node *)kvdef->kv_expr);

		if (kvdef->kv_subfields != NIL)
		{
			count = __setup_session_kvars_defs_array(vslot_desc_root,
													 vslot_desc_base,
													 kvdef->kv_subfields);
			vs_desc->idx_subfield = (vslot_desc_base - vslot_desc_root);
			vs_desc->num_subfield = count;

			vslot_desc_base += count;
			nitems += count;
		}
		vs_desc++;
	}
	return nitems;
}

static void
__build_session_kvars_defs(pgstromTaskState *pts,
						   kern_session_info *session,
						   StringInfo buf)
{
	pgstromPlanInfo *pp_info = pts->pp_info;
	kern_varslot_desc *kvars_defs;
	uint32_t	nitems = list_length(pp_info->kvars_deflist);
	uint32_t	nrooms = nitems;
	uint32_t	sz;
	ListCell   *lc;

	foreach (lc, pp_info->kvars_deflist)
	{
		codegen_kvar_defitem *kvdef = lfirst(lc);

		nrooms += __count_session_kvars_defs_subfields(kvdef);
	}
	sz = sizeof(kern_varslot_desc) * nrooms;
	
	kvars_defs = alloca(sz);
	memset(kvars_defs, 0, sz);
	__setup_session_kvars_defs_array(kvars_defs,
									 kvars_defs,
									 pp_info->kvars_deflist);
	session->kcxt_kvars_nrooms = nrooms;
	session->kcxt_kvars_nslots = nitems;
	session->kcxt_kvars_defs = __appendBinaryStringInfo(buf, kvars_defs, sz);
}

static uint32_t
__build_session_xact_state(StringInfo buf)
{
	Size		bufsz;
	char	   *buffer;

	bufsz = EstimateTransactionStateSpace();
	buffer = alloca(bufsz);
	SerializeTransactionState(bufsz, buffer);

	return __appendBinaryStringInfo(buf, buffer, bufsz);
}

static uint32_t
__build_session_timezone(StringInfo buf)
{
	uint32_t	offset = 0;

	if (session_timezone)
	{
		offset = __appendBinaryStringInfo(buf, session_timezone,
										  sizeof(struct pg_tz));
	}
	return offset;
}

static uint32_t
__build_session_encode(StringInfo buf)
{
	xpu_encode_info encode;

	memset(&encode, 0, sizeof(xpu_encode_info));
	strncpy(encode.encname,
			GetDatabaseEncodingName(),
			sizeof(encode.encname));
	encode.enc_maxlen = pg_database_encoding_max_length();
	encode.enc_mblen = NULL;

	return __appendBinaryStringInfo(buf, &encode, sizeof(xpu_encode_info));
}

static void
__build_session_lconvert(kern_session_info *session)
{
	struct lconv *lconvert = PGLC_localeconv();

	/* see comments about frac_digits in cash_in() */
	session->session_currency_frac_digits = lconvert->frac_digits;
}

const XpuCommand *
pgstromBuildSessionInfo(pgstromTaskState *pts,
						uint32_t join_inner_handle,
						TupleDesc groupby_tdesc_final)
{
	pgstromSharedState *ps_state = pts->ps_state;
	pgstromPlanInfo *pp_info = pts->pp_info;
	ExprContext	   *econtext = pts->css.ss.ps.ps_ExprContext;
	ParamListInfo	param_info = econtext->ecxt_param_list_info;
	uint32_t		nparams = (param_info ? param_info->numParams : 0);
	uint32_t		session_sz;
	kern_session_info *session;
	XpuCommand	   *xcmd;
	StringInfoData	buf;
	bytea		   *xpucode;

	initStringInfo(&buf);
	session_sz = offsetof(kern_session_info, poffset[nparams]);
	session = alloca(session_sz);
	memset(session, 0, session_sz);
	__appendZeroStringInfo(&buf, session_sz);
	if (param_info)
		__build_session_param_info(pts, session, &buf);
	if (pp_info->kvars_deflist != NIL)
		__build_session_kvars_defs(pts, session, &buf);
	if (pp_info->kexp_load_vars_packed)
	{
		xpucode = pp_info->kexp_load_vars_packed;
		session->xpucode_load_vars_packed =
			__appendBinaryStringInfo(&buf,
									 VARDATA(xpucode),
									 VARSIZE(xpucode) - VARHDRSZ);
	}
	if (pp_info->kexp_move_vars_packed)
	{
		xpucode = pp_info->kexp_move_vars_packed;
		session->xpucode_move_vars_packed =
			__appendBinaryStringInfo(&buf,
									 VARDATA(xpucode),
									 VARSIZE(xpucode) - VARHDRSZ);
	}
	if (pp_info->kexp_scan_quals)
	{
		xpucode = pp_info->kexp_scan_quals;
		session->xpucode_scan_quals =
			__appendBinaryStringInfo(&buf,
									 VARDATA(xpucode),
									 VARSIZE(xpucode) - VARHDRSZ);
	}
	if (pp_info->kexp_join_quals_packed)
	{
		xpucode = pp_info->kexp_join_quals_packed;
		session->xpucode_join_quals_packed =
			__appendBinaryStringInfo(&buf,
									 VARDATA(xpucode),
									 VARSIZE(xpucode) - VARHDRSZ);
	}
	if (pp_info->kexp_hash_keys_packed)
	{
		xpucode = pp_info->kexp_hash_keys_packed;
		session->xpucode_hash_values_packed =
			__appendBinaryStringInfo(&buf,
									 VARDATA(xpucode),
									 VARSIZE(xpucode) - VARHDRSZ);
	}
	if (pp_info->kexp_gist_evals_packed)
	{
		xpucode = pp_info->kexp_gist_evals_packed;
		session->xpucode_gist_evals_packed =
			__appendBinaryStringInfo(&buf,
									 VARDATA(xpucode),
									 VARSIZE(xpucode) - VARHDRSZ);
	}
	if (pp_info->kexp_projection)
	{
		xpucode = pp_info->kexp_projection;
		session->xpucode_projection =
			__appendBinaryStringInfo(&buf,
									 VARDATA(xpucode),
									 VARSIZE(xpucode) - VARHDRSZ);
	}
	if (pp_info->kexp_groupby_keyhash)
	{
		xpucode = pp_info->kexp_groupby_keyhash;
		session->xpucode_groupby_keyhash =
			__appendBinaryStringInfo(&buf,
									 VARDATA(xpucode),
									 VARSIZE(xpucode) - VARHDRSZ);
	}
	if (pp_info->kexp_groupby_keyload)
	{
		xpucode = pp_info->kexp_groupby_keyload;
		session->xpucode_groupby_keyload =
			__appendBinaryStringInfo(&buf,
									 VARDATA(xpucode),
									 VARSIZE(xpucode) - VARHDRSZ);
	}
	if (pp_info->kexp_groupby_keycomp)
	{
		xpucode = pp_info->kexp_groupby_keycomp;
		session->xpucode_groupby_keycomp =
			__appendBinaryStringInfo(&buf,
									 VARDATA(xpucode),
									 VARSIZE(xpucode) - VARHDRSZ);
	}
	if (pp_info->kexp_groupby_actions)
	{
		xpucode = pp_info->kexp_groupby_actions;
		session->xpucode_groupby_actions =
			__appendBinaryStringInfo(&buf,
									 VARDATA(xpucode),
									 VARSIZE(xpucode) - VARHDRSZ);
	}
	if (groupby_tdesc_final)
	{
		size_t		sz = estimate_kern_data_store(groupby_tdesc_final);
		kern_data_store *kds_temp = (kern_data_store *)alloca(sz);
		char		format = KDS_FORMAT_ROW;
		uint32_t	hash_nslots = 0;
		size_t		kds_length = (4UL << 20);	/* 4MB */

		if (pp_info->kexp_groupby_keyhash &&
			pp_info->kexp_groupby_keyload &&
			pp_info->kexp_groupby_keycomp)
		{
			double	n_groups = pts->css.ss.ps.plan->plan_rows;

			format = KDS_FORMAT_HASH;
			if (n_groups <= 5000.0)
				hash_nslots = 20000;
			else if (n_groups <= 4000000.0)
				hash_nslots = 20000 + (int)(2.0 * n_groups);
			else
				hash_nslots = 8020000 + n_groups;
			kds_length = (1UL << 30);			/* 1GB */
		}
		setup_kern_data_store(kds_temp, groupby_tdesc_final, kds_length, format);
		kds_temp->hash_nslots = hash_nslots;
		session->groupby_kds_final = __appendBinaryStringInfo(&buf, kds_temp, sz);
		session->groupby_prepfn_bufsz = pp_info->groupby_prepfn_bufsz;
		session->groupby_ngroups_estimation = pts->css.ss.ps.plan->plan_rows;
	}
	/* other database session information */
	session->query_plan_id = ps_state->query_plan_id;
	session->kcxt_kvecs_bufsz = pp_info->kvecs_bufsz;
	session->kcxt_kvecs_ndims = pp_info->kvecs_ndims;
	session->kcxt_extra_bufsz = pp_info->extra_bufsz;
	session->cuda_stack_size  = pp_info->cuda_stack_size;
	session->xpu_task_flags = pts->xpu_task_flags;
	session->hostEpochTimestamp = SetEpochTimestamp();
	session->xactStartTimestamp = GetCurrentTransactionStartTimestamp();
	session->session_xact_state = __build_session_xact_state(&buf);
	session->session_timezone = __build_session_timezone(&buf);
	session->session_encode = __build_session_encode(&buf);
	__build_session_lconvert(session);
	session->pgsql_port_number = PostPortNumber;
	session->pgsql_plan_node_id = pts->css.ss.ps.plan->plan_node_id;
	session->join_inner_handle = join_inner_handle;
	memcpy(buf.data, session, session_sz);

	/* setup XpuCommand */
	xcmd = palloc(offsetof(XpuCommand, u.session) + buf.len);
	memset(xcmd, 0, offsetof(XpuCommand, u.session));
	xcmd->magic = XpuCommandMagicNumber;
	xcmd->tag = XpuCommandTag__OpenSession;
	xcmd->length = offsetof(XpuCommand, u.session) + buf.len;
	memcpy(&xcmd->u.session, buf.data, buf.len);
	pfree(buf.data);

	return xcmd;
}

/*
 * pgstromTaskStateBeginScan
 */
static bool
pgstromTaskStateBeginScan(pgstromTaskState *pts)
{
	pgstromSharedState *ps_state = pts->ps_state;
	XpuConnection  *conn = pts->conn;
	uint32_t		curval, newval;

	Assert(conn != NULL);
	curval = pg_atomic_read_u32(&ps_state->parallel_task_control);
	do {
		if ((curval & 1) != 0)
			return false;
		newval = curval + 2;
	} while (!pg_atomic_compare_exchange_u32(&ps_state->parallel_task_control,
											 &curval, newval));
	pg_atomic_fetch_add_u32(&pts->rjoin_devs_count[conn->dev_index], 1);
	return true;
}

/*
 * pgstromTaskStateEndScan
 */
static bool
pgstromTaskStateEndScan(pgstromTaskState *pts, kern_final_task *kfin)
{
	pgstromSharedState *ps_state = pts->ps_state;
	XpuConnection  *conn = pts->conn;
	uint32_t		curval, newval;

	Assert(conn != NULL);
	memset(kfin, 0, sizeof(kern_final_task));
	curval = pg_atomic_read_u32(&ps_state->parallel_task_control);
	do {
		Assert(curval >= 2);
		newval = ((curval - 2) | 1);
	} while (!pg_atomic_compare_exchange_u32(&ps_state->parallel_task_control,
											 &curval, newval));
	if (newval == 1)
		kfin->final_plan_node = true;

	if (pg_atomic_sub_fetch_u32(&pts->rjoin_devs_count[conn->dev_index], 1) == 0)
		kfin->final_this_device = true;

	return (kfin->final_plan_node | kfin->final_this_device);
}

/*
 * pgstromTaskStateResetScan
 */
static void
pgstromTaskStateResetScan(pgstromTaskState *pts)
{
	pgstromSharedState *ps_state = pts->ps_state;
	int		num_devs = 0;

	/*
	 * pgstromExecTaskState() is never called on the single process
	 * execution, thus we have no state to reset.
	 */
	if (!ps_state)
		return;

	if ((pts->xpu_task_flags & DEVKIND__NVIDIA_GPU) != 0)
		num_devs = numGpuDevAttrs;
	else if ((pts->xpu_task_flags & DEVKIND__NVIDIA_DPU) != 0)
		num_devs = DpuStorageEntryCount();
	else
		elog(ERROR, "Bug? no GPU/DPUs are in use");

	pg_atomic_write_u32(&ps_state->parallel_task_control, 0);
	pg_atomic_write_u32(pts->rjoin_exit_count, 0);
	for (int i=0; i < num_devs; i++)
		pg_atomic_write_u32(pts->rjoin_devs_count + i, 0);
	if (pts->arrow_state)
	{
		pgstromArrowFdwExecReset(pts->arrow_state);
	}
	else if (ps_state->ss_handle == DSM_HANDLE_INVALID)
	{
		TableScanDesc scan = pts->css.ss.ss_currentScanDesc;

		table_rescan(scan, NULL);
	}
	else
	{
		Relation	rel = pts->css.ss.ss_currentRelation;
		ParallelTableScanDesc pscan = (ParallelTableScanDesc)
			((char *)ps_state + ps_state->parallel_scan_desc_offset);

		table_parallelscan_reinitialize(rel, pscan);
	}
}

/*
 * __updateStatsXpuCommand
 */
static void
__updateStatsXpuCommand(pgstromTaskState *pts, const XpuCommand *xcmd)
{
	if (xcmd->tag == XpuCommandTag__Success)
	{
		pgstromSharedState *ps_state = pts->ps_state;
		int		n_rels = Min(pts->num_rels, xcmd->u.results.num_rels);

		pg_atomic_fetch_add_u64(&ps_state->npages_direct_read,
								xcmd->u.results.npages_direct_read);
		pg_atomic_fetch_add_u64(&ps_state->npages_vfs_read,
								xcmd->u.results.npages_vfs_read);
		pg_atomic_fetch_add_u64(&ps_state->source_ntuples_raw,
								xcmd->u.results.nitems_raw);
		pg_atomic_fetch_add_u64(&ps_state->source_ntuples_in,
								xcmd->u.results.nitems_in);
		for (int i=0; i < n_rels; i++)
		{
			pg_atomic_fetch_add_u64(&ps_state->inners[i].stats_gist,
									xcmd->u.results.stats[i].nitems_gist);
			pg_atomic_fetch_add_u64(&ps_state->inners[i].stats_join,
									xcmd->u.results.stats[i].nitems_out);
		}
		pg_atomic_fetch_add_u64(&ps_state->result_ntuples, xcmd->u.results.nitems_out);
	}
	else if (xcmd->tag == XpuCommandTag__CPUFallback)
	{
		pgstromSharedState *ps_state = pts->ps_state;

		pg_atomic_fetch_add_u64(&ps_state->npages_direct_read,
								xcmd->u.fallback.npages_direct_read);
		pg_atomic_fetch_add_u64(&ps_state->npages_vfs_read,
								xcmd->u.fallback.npages_vfs_read);
	}
}

/*
 * __pickupNextXpuCommand
 *
 * MEMO: caller must hold 'conn->mutex'
 */
static XpuCommand *
__pickupNextXpuCommand(XpuConnection *conn)
{
	XpuCommand	   *xcmd;
	dlist_node	   *dnode;

	Assert(conn->num_ready_cmds > 0);
	dnode = dlist_pop_head_node(&conn->ready_cmds_list);
	xcmd = dlist_container(XpuCommand, chain, dnode);
	dlist_push_tail(&conn->active_cmds_list, &xcmd->chain);
	conn->num_ready_cmds--;

	return xcmd;
}

static XpuCommand *
__waitAndFetchNextXpuCommand(pgstromTaskState *pts, bool try_final_callback)
{
	XpuConnection  *conn = pts->conn;
	XpuCommand	   *xcmd;
	struct iovec	xcmd_iov[10];
	int				xcmd_iovcnt;
	int				ev;

	pthreadMutexLock(&conn->mutex);
	for (;;)
	{
		ResetLatch(MyLatch);

		/* device error checks */
		if (conn->errorbuf.errcode != ERRCODE_STROM_SUCCESS)
		{
			pthreadMutexUnlock(&conn->mutex);
			ereport(ERROR,
					(errcode(conn->errorbuf.errcode),
					 errmsg("%s:%d  %s",
							conn->errorbuf.filename,
							conn->errorbuf.lineno,
							conn->errorbuf.message),
					 errhint("device at %s, function at %s",
							 conn->devname,
							 conn->errorbuf.funcname)));

		}
		if (!dlist_is_empty(&conn->ready_cmds_list))
		{
			/* ok, ready commands we have */
			break;
		}
		else if (conn->num_running_cmds > 0)
		{
			/* wait for the running commands */
			pthreadMutexUnlock(&conn->mutex);
		}
		else
		{
			pthreadMutexUnlock(&conn->mutex);
			if (!pts->final_done)
			{
				kern_final_task	kfin;

				pts->final_done = true;
				if (try_final_callback &&
					pgstromTaskStateEndScan(pts, &kfin) &&
					pts->cb_final_chunk != NULL)
				{
					xcmd = pts->cb_final_chunk(pts, &kfin, xcmd_iov, &xcmd_iovcnt);
					if (xcmd)
					{
						xpuClientSendCommandIOV(conn, xcmd_iov, xcmd_iovcnt);
						continue;
					}
				}
			}
			return NULL;
		}
		CHECK_FOR_INTERRUPTS();

		ev = WaitLatch(MyLatch,
					   WL_LATCH_SET |
					   WL_TIMEOUT |
					   WL_POSTMASTER_DEATH,
					   1000L,
					   PG_WAIT_EXTENSION);
		if (ev & WL_POSTMASTER_DEATH)
			ereport(FATAL,
					(errcode(ERRCODE_ADMIN_SHUTDOWN),
					 errmsg("Unexpected Postmaster dead")));
		pthreadMutexLock(&conn->mutex);
	}
	xcmd = __pickupNextXpuCommand(conn);
	pthreadMutexUnlock(&conn->mutex);
	__updateStatsXpuCommand(pts, xcmd);
	return xcmd;
}

static XpuCommand *
__fetchNextXpuCommand(pgstromTaskState *pts) // QQQ 队列 这里隔离了两套执行器
{
	XpuConnection  *conn = pts->conn;
	XpuCommand	   *xcmd;
	struct iovec	xcmd_iov[10];
	int				xcmd_iovcnt;
	int				ev;
	int				max_async_tasks = pgstrom_max_async_tasks();

	while (!pts->scan_done)
	{
		CHECK_FOR_INTERRUPTS();

		pthreadMutexLock(&conn->mutex);
		/* device error checks */
		if (conn->errorbuf.errcode != ERRCODE_STROM_SUCCESS)
		{
			pthreadMutexUnlock(&conn->mutex);
			ereport(ERROR,
					(errcode(conn->errorbuf.errcode),
					 errmsg("%s:%d  %s",
							conn->errorbuf.filename,
							conn->errorbuf.lineno,
							conn->errorbuf.message),
					 errhint("device at %s, function at %s",
							 conn->devname,
							 conn->errorbuf.funcname)));
		}

		if ((conn->num_running_cmds + conn->num_ready_cmds) < max_async_tasks &&
			(dlist_is_empty(&conn->ready_cmds_list) ||
			 conn->num_running_cmds < max_async_tasks / 2))
		{
			/*
			 * xPU service still has margin to enqueue new commands.
			 * If we have no ready commands or number of running commands
			 * are less than pg_strom.max_async_tasks/2, we try to load
			 * the next chunk and enqueue this command.
			 */
			pthreadMutexUnlock(&conn->mutex);
			xcmd = pts->cb_next_chunk(pts, xcmd_iov, &xcmd_iovcnt);
			if (!xcmd)
			{
				Assert(pts->scan_done);
				break;
			}
			xpuClientSendCommandIOV(conn, xcmd_iov, xcmd_iovcnt);
		}
		else if (!dlist_is_empty(&conn->ready_cmds_list))
		{
			xcmd = __pickupNextXpuCommand(conn);
			pthreadMutexUnlock(&conn->mutex);
			__updateStatsXpuCommand(pts, xcmd);
			return xcmd;
		}
		else if (conn->num_running_cmds > 0)
		{
			/*
			 * This block means we already runs enough number of concurrent
			 * tasks, but none of them are already finished.
			 * So, let's wait for the response.
			 */
			ResetLatch(MyLatch);
			pthreadMutexUnlock(&conn->mutex);

			ev = WaitLatch(MyLatch,
						   WL_LATCH_SET |
						   WL_TIMEOUT |
						   WL_POSTMASTER_DEATH,
						   1000L,
						   PG_WAIT_EXTENSION);
			if (ev & WL_POSTMASTER_DEATH)
				ereport(FATAL,
						(errcode(ERRCODE_ADMIN_SHUTDOWN),
						 errmsg("Unexpected Postmaster dead")));
		}
		else
		{
			/*
			 * Unfortunately, we touched the threshold. Take a short wait
			 */
			pthreadMutexUnlock(&conn->mutex);
			pg_usleep(20000L);		/* 20ms */
		}
	}
	return __waitAndFetchNextXpuCommand(pts, true);
}

/*
 * pgstromScanNextTuple
 */
static TupleTableSlot *
pgstromScanNextTuple(pgstromTaskState *pts)
{
	TupleTableSlot *slot = pts->css.ss.ss_ScanTupleSlot;

	for (;;)
	{
		kern_data_store *kds = pts->curr_kds;
		int64_t		index = pts->curr_index++;

		if (index < kds->nitems)
		{
			kern_tupitem   *tupitem = KDS_GET_TUPITEM(kds, index);

			pts->curr_htup.t_len = tupitem->t_len;
			memcpy(&pts->curr_htup.t_self,
				   &tupitem->htup.t_ctid,
				   sizeof(ItemPointerData));
			pts->curr_htup.t_data = &tupitem->htup;
			return ExecStoreHeapTuple(&pts->curr_htup, slot, false);
		}
		if (++pts->curr_chunk < pts->curr_resp->u.results.chunks_nitems)
		{
			pts->curr_kds = (kern_data_store *)((char *)kds + kds->length);
			pts->curr_index = 0;
			continue;
		}
		return NULL;
	}
}

/*
 * pgstromExecFinalChunk
 */
static XpuCommand *
pgstromExecFinalChunk(pgstromTaskState *pts,
					  kern_final_task *kfin,
					  struct iovec *xcmd_iov, int *xcmd_iovcnt)
{
	XpuCommand	   *xcmd;

	pts->xcmd_buf.len = offsetof(XpuCommand, u.fin.data);
	enlargeStringInfo(&pts->xcmd_buf, 0);

	xcmd = (XpuCommand *)pts->xcmd_buf.data;
	memset(xcmd, 0, sizeof(XpuCommand));
	xcmd->magic  = XpuCommandMagicNumber;
	xcmd->tag    = XpuCommandTag__XpuTaskFinal;
	xcmd->length = offsetof(XpuCommand, u.fin.data);
	memcpy(&xcmd->u.fin, kfin, sizeof(kern_final_task));

	xcmd_iov[0].iov_base = xcmd;
	xcmd_iov[0].iov_len  = offsetof(XpuCommand, u.fin.data);
	*xcmd_iovcnt = 1;

	return xcmd;
}

/*
 * pgstromExecFinalChunkDummy
 *
 * In case of xPU-JOIN without RIGHT OUTER, this handler inject an empty
 * XpuCommandTag__Success command on the tail of ready list just to increment
 * pts->rjoin_exit_count.
 */
static XpuCommand *
pgstromExecFinalChunkDummy(pgstromTaskState *pts,
						   kern_final_task *kfin,
						   struct iovec *xcmd_iov, int *xcmd_iovcnt)
{
	XpuConnection  *conn = pts->conn;
	XpuCommand	   *xcmd;

	if (kfin->final_plan_node)
	{
		xcmd = __xpuConnectAllocCommand(conn, sizeof(XpuCommand));
		if (!xcmd)
			elog(ERROR, "out of memory");
		memset(xcmd, 0, sizeof(XpuCommand));
		xcmd->magic = XpuCommandMagicNumber;
		xcmd->tag = XpuCommandTag__Success;
		xcmd->length = offsetof(XpuCommand, u.results.stats);
		xcmd->priv = conn;
		xcmd->u.results.final_plan_node = true;

		/* attach dummy xcmd at the tail of ready list */
		pthreadMutexLock(&conn->mutex);
		dlist_push_tail(&conn->ready_cmds_list, &xcmd->chain);
		conn->num_ready_cmds++;
		SetLatch(MyLatch);
		pthreadMutexUnlock(&conn->mutex);
	}
	return NULL;
}

/*
 * CPU Fallback Routines
 */
static void
ExecFallbackRowDataStore(pgstromTaskState *pts,
						 kern_data_store *kds)
{
	for (uint32_t i=0; i < kds->nitems; i++)
	{
		kern_tupitem   *tupitem = KDS_GET_TUPITEM(kds, i);
		HeapTupleData	tuple;

		tuple.t_len = tupitem->t_len;
		ItemPointerCopy(&tupitem->htup.t_ctid, &tuple.t_self);
		tuple.t_tableOid = kds->table_oid;
		tuple.t_data = &tupitem->htup;
		pts->cb_cpu_fallback(pts, &tuple);
	}
}

static void
ExecFallbackBlockDataStore(pgstromTaskState *pts,
						   kern_data_store *kds)
{
	for (uint32_t i=0; i < kds->nitems; i++)
	{
		PageHeaderData *pg_page = KDS_BLOCK_PGPAGE(kds, i);
		BlockNumber		block_nr = KDS_BLOCK_BLCKNR(kds, i);
		uint32_t		ntuples = PageGetMaxOffsetNumber((Page)pg_page);

		for (uint32_t k=0; k < ntuples; k++)
		{
			ItemIdData	   *lpp = &pg_page->pd_linp[k];
			HeapTupleData	tuple;

			if (ItemIdIsNormal(lpp))
			{
				tuple.t_len = ItemIdGetLength(lpp);
				tuple.t_self.ip_blkid.bi_hi = (uint16_t)(block_nr >> 16);
				tuple.t_self.ip_blkid.bi_lo = (uint16_t)(block_nr & 0xffffU);
				tuple.t_self.ip_posid = k+1;
				tuple.t_tableOid = kds->table_oid;
				tuple.t_data = (HeapTupleHeader)PageGetItem((Page)pg_page, lpp);

				pts->cb_cpu_fallback(pts, &tuple);
			}
		}
	}
}

static void
ExecFallbackColumnDataStore(pgstromTaskState *pts,
							kern_data_store *kds)
{
	Relation		rel = pts->css.ss.ss_currentRelation;
	EState		   *estate = pts->css.ss.ps.state;
	TableScanDesc	scan;

	scan = table_beginscan(rel, estate->es_snapshot, 0, NULL);
	while (table_scan_getnextslot(scan, ForwardScanDirection, pts->base_slot))
	{
		HeapTuple	tuple;
		bool		should_free;

		tuple = ExecFetchSlotHeapTuple(pts->base_slot, false, &should_free);
		pts->cb_cpu_fallback(pts, tuple);
		if (should_free)
			pfree(tuple);
	}
	table_endscan(scan);
}

static void
ExecFallbackArrowDataStore(pgstromTaskState *pts,
						   kern_data_store *kds)
{
	pgstromPlanInfo *pp_info = pts->pp_info;

	for (uint32_t index=0; index < kds->nitems; index++)
	{
		HeapTuple	tuple;
		bool		should_free;

		if (!kds_arrow_fetch_tuple(pts->base_slot,
								   kds, index,
								   pp_info->outer_refs))
			break;

		tuple = ExecFetchSlotHeapTuple(pts->base_slot, false, &should_free);
		pts->cb_cpu_fallback(pts, tuple);
		if (should_free)
			pfree(tuple);
	}
}

/*
 * __setupTaskStateRequestBuffer
 */
static void
__setupTaskStateRequestBuffer(pgstromTaskState *pts,
							  TupleDesc tdesc_src,
							  TupleDesc tdesc_dst,
							  char format)
{
	XpuCommand	   *xcmd;
	kern_data_store *kds;
	size_t			bufsz;
	size_t			off;

	initStringInfo(&pts->xcmd_buf);
	bufsz = MAXALIGN(offsetof(XpuCommand, u.task.data));
	if (pts->gcache_desc)
		bufsz += MAXALIGN(sizeof(GpuCacheIdent));
	if (tdesc_src)
		bufsz += estimate_kern_data_store(tdesc_src);
	if (tdesc_dst)
		bufsz += estimate_kern_data_store(tdesc_dst);
	enlargeStringInfo(&pts->xcmd_buf, bufsz);

	xcmd = (XpuCommand *)pts->xcmd_buf.data;
	memset(xcmd, 0, offsetof(XpuCommand, u.task.data));
	off = offsetof(XpuCommand, u.task.data);
	if (pts->gcache_desc)
	{
		const GpuCacheIdent *ident = getGpuCacheDescIdent(pts->gcache_desc);

		memcpy((char *)xcmd + off, ident, sizeof(GpuCacheIdent));
		off += MAXALIGN(sizeof(GpuCacheIdent));
	}
	if (tdesc_dst)
	{
		xcmd->u.task.kds_dst_offset = off;
		kds  = (kern_data_store *)((char *)xcmd + off);
		off += setup_kern_data_store(kds, tdesc_dst, 0, KDS_FORMAT_ROW);
	}
	if (tdesc_src)
	{
		xcmd->u.task.kds_src_offset = off;
		kds  = (kern_data_store *)((char *)xcmd + off);
		off += setup_kern_data_store(kds, tdesc_src, 0, format);
	}
	Assert(off <= bufsz);
	xcmd->magic = XpuCommandMagicNumber;
	xcmd->tag   = (!pts->gcache_desc
				   ? XpuCommandTag__XpuTaskExec
				   : XpuCommandTag__XpuTaskExecGpuCache);
	xcmd->length = off;
	pts->xcmd_buf.len = off;
}



/*
 * __execInitTaskStateCpuFallback
 *
 * CPU fallback process moves the tuple as follows
 *
 * <Base Relation> (can be both of heap and arrow)
 *      |
 *      v
 * [Base Slot]  ... equivalent to the base relation
 *      |
 *      v
 * (Base Quals) ... WHERE-clause
 *      |
 *      +-----------+
 *      |           |
 *      |           v
 *      |    [Fallback Slot]
 *      |           |
 *      |           v
 *      |      ((GPU-Join)) ... Join for each depth
 *      |           |
 *      |           v
 *      |     [Fallback Slot]
 *      |           |
 *      v           v
 * (Fallback Projection)  ... pts->fallback_proj
 *          |
 *          v
 *  [ CustomScan Slot ]   ... Equivalent to GpuProj/GpuPreAgg results
 */
static Node *
__fixup_fallback_projection(Node *node, void *__data)
{
	pgstromPlanInfo *pp_info = __data;
	ListCell   *lc;

	if (!node)
		return NULL;
	foreach (lc, pp_info->kvars_deflist)
	{
		codegen_kvar_defitem *kvdef = lfirst(lc);

		if (equal(kvdef->kv_expr, node))
		{
			return (Node *)makeVar(INDEX_VAR,
								   kvdef->kv_slot_id + 1,
								   exprType(node),
								   exprTypmod(node),
								   exprCollation(node),
								   0);
		}
	}

	if (IsA(node, Var))
	{
		Var	   *var = (Var *)node;

		return (Node *)makeNullConst(var->vartype,
									 var->vartypmod,
									 var->varcollid);
	}
	return expression_tree_mutator(node, __fixup_fallback_projection, pp_info);
}

/*
 * fixup_fallback_join_inner_keys
 */
static void
__execInitTaskStateCpuFallback(pgstromTaskState *pts)
{
	pgstromPlanInfo *pp_info = pts->pp_info;
	CustomScan *cscan = (CustomScan *)pts->css.ss.ps.plan;
	Relation	rel = pts->css.ss.ss_currentRelation;
	TupleDesc	fallback_tdesc;
	List	   *cscan_tlist = NIL;
	ListCell   *lc;

	/*
	 * Init scan-quals for the base relation
	 */
	pts->base_quals = ExecInitQual(pp_info->scan_quals_fallback,
								   &pts->css.ss.ps);
	pts->base_slot = MakeSingleTupleTableSlot(RelationGetDescr(rel),
											  table_slot_callbacks(rel));
	if (pts->num_rels == 0)
	{
		/*
		 * GpuScan can bypass fallback_slot, so fallback_proj directly
		 * transform the base_slot to ss_ScanTupleSlot.
		 */
		bool	meet_junk = false;

		foreach (lc, cscan->custom_scan_tlist)
		{
			TargetEntry *tle = lfirst(lc);

			if (tle->resjunk)
				meet_junk = true;
			else if (meet_junk)
				elog(ERROR, "Bug? custom_scan_tlist has valid attribute after junk");
			else
				cscan_tlist = lappend(cscan_tlist, tle);
		}
		fallback_tdesc = RelationGetDescr(rel);
	}
	else
	{
		/*
		 * CpuJoin runs its multi-level join on the fallback-slot that
		 * follows the kvars_slot layout.
		 * Then, it works as a source of fallback_proj.
		 */
		int		nslots = list_length(pp_info->kvars_deflist);
		bool	meet_junk = false;

		fallback_tdesc = CreateTemplateTupleDesc(nslots);
		foreach (lc, pp_info->kvars_deflist)
		{
			codegen_kvar_defitem *kvdef = lfirst(lc);

			TupleDescInitEntry(fallback_tdesc,
							   kvdef->kv_slot_id + 1,
							   psprintf("KVAR_%u", kvdef->kv_slot_id),
							   exprType((Node *)kvdef->kv_expr),
							   exprTypmod((Node *)kvdef->kv_expr),
							   0);
		}
		pts->fallback_slot = MakeSingleTupleTableSlot(fallback_tdesc,
													  &TTSOpsVirtual);
		foreach (lc, cscan->custom_scan_tlist)
		{
			TargetEntry *tle = lfirst(lc);

			if (tle->resjunk)
				meet_junk = true;
			else if (meet_junk)
				elog(ERROR, "Bug? custom_scan_tlist has valid attribute after junk");
			else
			{
				TargetEntry	*__tle = (TargetEntry *)
					__fixup_fallback_projection((Node *)tle, pp_info);
				cscan_tlist = lappend(cscan_tlist, __tle);
			}
		}
	}
	pts->fallback_proj =
		ExecBuildProjectionInfo(cscan_tlist,
								pts->css.ss.ps.ps_ExprContext,
								pts->css.ss.ss_ScanTupleSlot,
								&pts->css.ss.ps,
								fallback_tdesc);
}

/*
 * pgstromExecInitTaskState
 */
void
pgstromExecInitTaskState(CustomScanState *node, EState *estate, int eflags)
{
	pgstromTaskState *pts = (pgstromTaskState *)node;
	pgstromPlanInfo	*pp_info = pts->pp_info;
	CustomScan *cscan = (CustomScan *)pts->css.ss.ps.plan;
	Relation	rel = pts->css.ss.ss_currentRelation;
	TupleDesc	tupdesc_src = RelationGetDescr(rel);
	TupleDesc	tupdesc_dst;
	int			depth_index = 0;
	bool		has_right_outer = false;
	ListCell   *lc;

	/* sanity checks */
	Assert(rel != NULL &&
		   outerPlanState(node) == NULL &&
		   innerPlanState(node) == NULL &&
		   pp_info->num_rels == list_length(cscan->custom_plans) &&
		   pts->num_rels == list_length(cscan->custom_plans));
	/*
	 * PG-Strom supports:
	 * - regular relation with 'heap' access method
	 * - foreign-table with 'arrow_fdw' driver
	 */
	if (RelationGetForm(rel)->relkind == RELKIND_RELATION ||
		RelationGetForm(rel)->relkind == RELKIND_MATVIEW)
	{
		SMgrRelation smgr = RelationGetSmgr(rel);
		Oid			am_oid = RelationGetForm(rel)->relam;
		const char *kds_pathname = smgr_relpath(smgr, MAIN_FORKNUM);

		if (am_oid != HEAP_TABLE_AM_OID)
			elog(ERROR, "PG-Strom does not support table access method: %s",
				 get_am_name(am_oid));
		/* setup GpuCache if any */
		if (pp_info->gpu_cache_dindex >= 0)
			pts->gcache_desc = pgstromGpuCacheExecInit(pts);
		/* setup BRIN-index if any */
		pgstromBrinIndexExecBegin(pts,
								  pp_info->brin_index_oid,
								  pp_info->brin_index_conds,
								  pp_info->brin_index_quals);
		if ((pts->xpu_task_flags & DEVKIND__NVIDIA_GPU) != 0)
			pts->optimal_gpus = GetOptimalGpuForRelation(rel);
		if ((pts->xpu_task_flags & DEVKIND__NVIDIA_DPU) != 0)
			pts->ds_entry = GetOptimalDpuForRelation(rel, &kds_pathname);
		pts->kds_pathname = kds_pathname;
	}
	else if (RelationGetForm(rel)->relkind == RELKIND_FOREIGN_TABLE)
	{
		if (!pgstromArrowFdwExecInit(pts,
									 pp_info->scan_quals_fallback,
									 pp_info->outer_refs))
			elog(ERROR, "Bug? only arrow_fdw is supported in PG-Strom");
	}
	else
	{
		elog(ERROR, "Bug? PG-Strom does not support relation type of '%s'",
			 RelationGetRelationName(rel));
	}

	/*
	 * Re-initialization of scan tuple-descriptor and projection-info,
	 * because commit 1a8a4e5cde2b7755e11bde2ea7897bd650622d3e of
	 * PostgreSQL makes to assign result of ExecTypeFromTL() instead
	 * of ExecCleanTypeFromTL; that leads incorrect projection.
	 * So, we try to remove junk attributes from the scan-descriptor.
	 *
	 * And, device projection returns a tuple in heap-format, so we
	 * prefer TTSOpsHeapTuple, instead of the TTSOpsVirtual.
	 */
	tupdesc_dst = ExecCleanTypeFromTL(cscan->custom_scan_tlist);
	ExecInitScanTupleSlot(estate, &pts->css.ss, tupdesc_dst,
						  &TTSOpsHeapTuple);
	ExecAssignScanProjectionInfoWithVarno(&pts->css.ss, INDEX_VAR);

	/*
	 * Initialize the CPU Fallback stuff
	 */
	__execInitTaskStateCpuFallback(pts);
	
	/*
	 * init inner relations
	 */
	depth_index = 0;
	foreach (lc, cscan->custom_plans)
	{
		pgstromTaskInnerState *istate = &pts->inners[depth_index];
		pgstromPlanInnerInfo *pp_inner = &pp_info->inners[depth_index];
		Plan	   *plan = lfirst(lc);
		ListCell   *cell;

		istate->ps = ExecInitNode(plan, estate, eflags);
		istate->econtext = CreateExprContext(estate);
		istate->depth = depth_index + 1;
		istate->join_type = pp_inner->join_type;
		istate->join_quals = ExecInitQual(pp_inner->join_quals_fallback,
										  &pts->css.ss.ps);
		istate->other_quals = ExecInitQual(pp_inner->other_quals_fallback,
										   &pts->css.ss.ps);
		if (pp_inner->join_type == JOIN_FULL ||
			pp_inner->join_type == JOIN_RIGHT)
			has_right_outer = true;

		foreach (cell, pp_inner->hash_outer_keys_fallback)
		{
			Node	   *outer_key = (Node *)lfirst(cell);
			ExprState  *es;
			devtype_info *dtype;

			dtype = pgstrom_devtype_lookup(exprType(outer_key));
			if (!dtype)
				elog(ERROR, "failed on lookup device type of %s",
					 nodeToString(outer_key));
			es = ExecInitExpr((Expr *)outer_key, &pts->css.ss.ps);
			istate->hash_outer_keys = lappend(istate->hash_outer_keys, es);
			istate->hash_outer_funcs = lappend(istate->hash_outer_funcs,
											   dtype->type_hashfunc);
		}
		/* inner hash-keys references the result of inner-slot */
		foreach (cell, pp_inner->hash_inner_keys_fallback)
		{
			Node	   *inner_key = (Node *)lfirst(cell);
			ExprState  *es;
			devtype_info *dtype;

			dtype = pgstrom_devtype_lookup(exprType(inner_key));
			if (!dtype)
				elog(ERROR, "failed on lookup device type of %s",
					 nodeToString(inner_key));
			es = ExecInitExpr((Expr *)inner_key, &pts->css.ss.ps);
			istate->hash_inner_keys = lappend(istate->hash_inner_keys, es);
			istate->hash_inner_funcs = lappend(istate->hash_inner_funcs,
											   dtype->type_hashfunc);
		}

		if (OidIsValid(pp_inner->gist_index_oid))
		{
			istate->gist_irel = index_open(pp_inner->gist_index_oid,
										   AccessShareLock);
			istate->gist_clause = ExecInitExpr((Expr *)pp_inner->gist_clause,
											   &pts->css.ss.ps);
			istate->gist_ctid_resno = pp_inner->gist_ctid_resno;
		}
		pts->css.custom_ps = lappend(pts->css.custom_ps, istate->ps);
		depth_index++;
	}
	Assert(depth_index == pts->num_rels);
	
	/*
	 * Setup request buffer // QQQ 输入入口
	 */
	if (pts->arrow_state)		/* Apache Arrow */
	{
		pts->cb_next_chunk = pgstromScanChunkArrowFdw; // QQQ 内部 CPU 数据入口
		pts->cb_next_tuple = pgstromScanNextTuple;
	    __setupTaskStateRequestBuffer(pts,
									  NULL,
									  tupdesc_dst,
									  KDS_FORMAT_ARROW);
	}
	else if (pts->gcache_desc)		/* GPU-Cache */
	{
		pts->cb_next_chunk = pgstromScanChunkGpuCache; // QQQ GPU cache 输入
		pts->cb_next_tuple = pgstromScanNextTuple;
		__setupTaskStateRequestBuffer(pts,
									  NULL,
									  tupdesc_dst,
									  KDS_FORMAT_COLUMN);
	}
	else if (!bms_is_empty(pts->optimal_gpus) ||	/* GPU-Direct SQL */
			 pts->ds_entry)							/* DPU Storage */
	{
		pts->cb_next_chunk = pgstromRelScanChunkDirect; // QQQ GPU direct 数据
		pts->cb_next_tuple = pgstromScanNextTuple;
		__setupTaskStateRequestBuffer(pts,
									  tupdesc_src,
									  tupdesc_dst,
									  KDS_FORMAT_BLOCK);
	}
	else						/* Slow normal heap storage */
	{
		pts->cb_next_chunk = pgstromRelScanChunkNormal; // QQQ heap 表数据
		pts->cb_next_tuple = pgstromScanNextTuple;
		__setupTaskStateRequestBuffer(pts,
									  tupdesc_src,
									  tupdesc_dst,
									  KDS_FORMAT_ROW);
	}

	/*
	 * workload specific callback routines
	 */
	if ((pts->xpu_task_flags & DEVTASK__SCAN) != 0)
	{
		pts->cb_cpu_fallback = ExecFallbackCpuScan;
	}
	else if ((pts->xpu_task_flags & DEVTASK__JOIN) != 0)
	{
		if (has_right_outer)
			pts->cb_final_chunk = pgstromExecFinalChunk;
		else
			pts->cb_final_chunk = pgstromExecFinalChunkDummy;
		pts->cb_cpu_fallback = ExecFallbackCpuJoin;
	}
	else if ((pts->xpu_task_flags & DEVTASK__PREAGG) != 0)
	{
		pts->cb_final_chunk = pgstromExecFinalChunk;
		pts->cb_cpu_fallback = ExecFallbackCpuPreAgg;
	}
	else
		elog(ERROR, "Bug? unknown DEVTASK");
	/* other fields init */
	pts->curr_vm_buffer = InvalidBuffer;
}

/*
 * pgstromExecScanAccess
 */
static TupleTableSlot *
pgstromExecScanAccess(pgstromTaskState *pts)
{
	TupleTableSlot *slot;
	XpuCommand	   *resp;

	slot = pgstromFetchFallbackTuple(pts);
	if (slot)
		return slot;

	while (!pts->curr_resp || !(slot = pts->cb_next_tuple(pts)))
	{
	next_chunks:
		if (pts->curr_resp)
			xpuClientPutResponse(pts->curr_resp);
		pts->curr_resp = __fetchNextXpuCommand(pts); // 有个队列
		if (!pts->curr_resp)
			return pgstromFetchFallbackTuple(pts);
		resp = pts->curr_resp;
		switch (resp->tag)
		{
			case XpuCommandTag__Success:
				if (resp->u.results.ojmap_offset != 0)
					ExecFallbackCpuJoinOuterJoinMap(pts, resp);
				if (resp->u.results.final_plan_node)
					ExecFallbackCpuJoinRightOuter(pts);
				if (resp->u.results.chunks_nitems == 0)
					goto next_chunks;
				pts->curr_kds = (kern_data_store *)
					((char *)resp + resp->u.results.chunks_offset);
				pts->curr_chunk = 0;
				pts->curr_index = 0;
				break;

			case XpuCommandTag__CPUFallback:
				elog(pgstrom_cpu_fallback_elevel,
					 "(%s:%d) CPU fallback due to %s [%s]",
					 resp->u.fallback.error.filename,
					 resp->u.fallback.error.lineno,
					 resp->u.fallback.error.message,
					 resp->u.fallback.error.funcname);
				switch (resp->u.fallback.kds_src.format)
				{
					case KDS_FORMAT_ROW:
						ExecFallbackRowDataStore(pts, &resp->u.fallback.kds_src);
						break;
					case KDS_FORMAT_BLOCK:
						ExecFallbackBlockDataStore(pts, &resp->u.fallback.kds_src);
						break;
					case KDS_FORMAT_COLUMN:
						ExecFallbackColumnDataStore(pts, &resp->u.fallback.kds_src);
						break;
					case KDS_FORMAT_ARROW:
						ExecFallbackArrowDataStore(pts, &resp->u.fallback.kds_src);
						break;
					default:
						elog(ERROR, "CPU fallback received unknown KDS format (%c)",
							 resp->u.fallback.kds_src.format);
						break;
				}
				goto next_chunks;

			default:
				elog(ERROR, "unknown response tag: %u", resp->tag);
				break;
		}
	}
	slot_getallattrs(slot);
	return slot;
}

/*
 * pgstromExecScanReCheck
 */
static TupleTableSlot *
pgstromExecScanReCheck(pgstromTaskState *pts, EPQState *epqstate)
{
	Index		scanrelid = ((Scan *)pts->css.ss.ps.plan)->scanrelid;

	/* see ExecScanFetch */
	if (scanrelid == 0)
	{
		elog(ERROR, "Bug? CustomScan(%s) has scanrelid==0",
			 pts->css.methods->CustomName);
	}
	else if (epqstate->relsubs_done[scanrelid-1])
	{
		return NULL;
	}
	else if (epqstate->relsubs_slot[scanrelid-1])
	{
		TupleTableSlot *ss_slot = pts->css.ss.ss_ScanTupleSlot;
		TupleTableSlot *epq_slot = epqstate->relsubs_slot[scanrelid-1];
		size_t			fallback_index_saved = pts->fallback_index;
		size_t			fallback_usage_saved = pts->fallback_usage;

		Assert(epqstate->relsubs_rowmark[scanrelid - 1] == NULL);
		/* Mark to remember that we shouldn't return it again */
		epqstate->relsubs_done[scanrelid - 1] = true;

		/* Return empty slot if we haven't got a test tuple */
		if (TupIsNull(epq_slot))
			ExecClearTuple(ss_slot);
		else
		{
			HeapTuple	epq_tuple;
			bool		should_free;
#if 0
			slot_getallattrs(epq_slot);
			for (int j=0; j < epq_slot->tts_nvalid; j++)
			{
				elog(INFO, "epq_slot[%d] isnull=%s values=0x%lx", j,
					 epq_slot->tts_isnull[j] ? "true" : "false",
					 epq_slot->tts_values[j]);
			}
#endif
			epq_tuple = ExecFetchSlotHeapTuple(epq_slot, false,
											   &should_free);
			if (pts->cb_cpu_fallback(pts, epq_tuple) &&
				pts->fallback_tuples != NULL &&
				pts->fallback_buffer != NULL &&
				pts->fallback_nitems > fallback_index_saved)
			{
				HeapTupleData	htup;
				kern_tupitem   *titem = (kern_tupitem *)
					(pts->fallback_buffer +
					 pts->fallback_tuples[fallback_index_saved]);

				htup.t_len = titem->t_len;
				htup.t_data = &titem->htup;
				ss_slot = pts->css.ss.ss_ScanTupleSlot;
				ExecForceStoreHeapTuple(&htup, ss_slot, false);
			}
			else
			{
				ExecClearTuple(ss_slot);
			}
			/* release fallback tuple & buffer */
			if (should_free)
				pfree(epq_tuple);
			pts->fallback_index = fallback_index_saved;
			pts->fallback_usage = fallback_usage_saved;
		}
		return ss_slot;
	}
	else if (epqstate->relsubs_rowmark[scanrelid-1])
	{
		elog(ERROR, "RowMark on CustomScan(%s) is not implemented yet",
			 pts->css.methods->CustomName);
	}
	return pgstromExecScanAccess(pts);
}

/*
 * __pgstromExecTaskOpenConnection
 */
static bool
__pgstromExecTaskOpenConnection(pgstromTaskState *pts)
{
	const XpuCommand *session;
	uint32_t	inner_handle = 0;
	TupleDesc	tupdesc_kds_final = NULL;

	/* attach pgstromSharedState, if none */
	if (!pts->ps_state)
		pgstromSharedStateInitDSM(&pts->css, NULL, NULL);
	/* preload inner buffer, if any */
	if (pts->num_rels > 0)
	{
		inner_handle = GpuJoinInnerPreload(pts);
		if (inner_handle == 0)
			return false;
	}
	/* XPU-PreAgg needs tupdesc of kds_final */
	if ((pts->xpu_task_flags & DEVTASK__PREAGG) != 0)
	{
		tupdesc_kds_final = pts->css.ss.ps.scandesc;
	}
	/* build the session information */
	session = pgstromBuildSessionInfo(pts, inner_handle, tupdesc_kds_final);

	if ((pts->xpu_task_flags & DEVKIND__NVIDIA_GPU) != 0)
	{
		gpuClientOpenSession(pts, session);
	}
	else if ((pts->xpu_task_flags & DEVKIND__NVIDIA_DPU) != 0)
	{
		Assert(pts->ds_entry != NULL);
		DpuClientOpenSession(pts, session);
	}
	else
	{
		elog(ERROR, "Bug? unknown PG-Strom task kind: %08x", pts->xpu_task_flags);
	}
	/* update the scan/join control variables */
	if (!pgstromTaskStateBeginScan(pts))
		return false;
	
	return true;
}

/*
 * pgstromExecTaskState
 */
TupleTableSlot *
pgstromExecTaskState(CustomScanState *node) // QQQ 入口 2
{
	pgstromTaskState *pts = (pgstromTaskState *)node;
	EState		   *estate = pts->css.ss.ps.state;
	ExprState	   *host_quals = node->ss.ps.qual;
	ExprContext	   *econtext = node->ss.ps.ps_ExprContext;
	ProjectionInfo *proj_info = node->ss.ps.ps_ProjInfo;
	TupleTableSlot *slot;

	if (!pts->conn)
	{
		if (!__pgstromExecTaskOpenConnection(pts))
			return NULL;
		Assert(pts->conn);
	}

	/*
	 * see, ExecScan() - it assumes CustomScan with scanrelid > 0 returns
	 * tuples identical with table definition, thus these tuples are not
	 * suitable for input of ExecProjection().
	 */
	if (estate->es_epq_active)
	{
		slot = pgstromExecScanReCheck(pts, estate->es_epq_active);
		if (TupIsNull(slot))
			return NULL;
		ResetExprContext(econtext);
		econtext->ecxt_scantuple = slot;
		if (proj_info)
			return ExecProject(proj_info);
		return slot;
	}

	for (;;)
	{
		slot = pgstromExecScanAccess(pts); // QQQ 入口3
		if (TupIsNull(slot))
			break;
		/* check whether the current tuple satisfies the qual-clause */
		ResetExprContext(econtext);
		econtext->ecxt_scantuple = slot;

		if (!host_quals || ExecQual(host_quals, econtext))
		{
			if (proj_info)
				return ExecProject(proj_info);
			return slot;
		}
		InstrCountFiltered1(pts, 1);
	}
	return NULL;
}

/*
 * pgstromExecEndTaskState
 */
void
pgstromExecEndTaskState(CustomScanState *node)
{
	pgstromTaskState   *pts = (pgstromTaskState *)node;
	pgstromSharedState *ps_state = pts->ps_state;
	ListCell   *lc;

	if (pts->curr_vm_buffer != InvalidBuffer)
		ReleaseBuffer(pts->curr_vm_buffer);
	if (pts->conn)
		xpuClientCloseSession(pts->conn);
	if (pts->br_state)
		pgstromBrinIndexExecEnd(pts);
	if (pts->gcache_desc)
		pgstromGpuCacheExecEnd(pts);
	if (pts->arrow_state)
		pgstromArrowFdwExecEnd(pts->arrow_state);
	if (pts->base_slot)
		ExecDropSingleTupleTableSlot(pts->base_slot);
	if (pts->css.ss.ss_currentScanDesc)
		table_endscan(pts->css.ss.ss_currentScanDesc);
	for (int i=0; i < pts->num_rels; i++)
	{
		pgstromTaskInnerState *istate = &pts->inners[i];

		if (istate->gist_irel)
			index_close(istate->gist_irel, AccessShareLock);
	}
	if (pts->h_kmrels)
		__munmapShmem(pts->h_kmrels);
	if (!IsParallelWorker())
	{
		if (ps_state && ps_state->preload_shmem_handle != 0)
			__shmemDrop(ps_state->preload_shmem_handle);
	}
	foreach (lc, pts->css.custom_ps)
		ExecEndNode((PlanState *) lfirst(lc));
}

/*
 * pgstromExecResetTaskState
 */
void
pgstromExecResetTaskState(CustomScanState *node)
{
	pgstromTaskState *pts = (pgstromTaskState *) node;

	if (pts->conn)
	{
		xpuClientCloseSession(pts->conn);
		pts->conn = NULL;
	}
	pgstromTaskStateResetScan(pts);
	if (pts->br_state)
		pgstromBrinIndexExecReset(pts);
	if (pts->arrow_state)
		pgstromArrowFdwExecReset(pts->arrow_state);
}

/*
 * pgstromSharedStateEstimateDSM
 */
Size
pgstromSharedStateEstimateDSM(CustomScanState *node,
							  ParallelContext *pcxt)
{
	pgstromTaskState *pts = (pgstromTaskState *)node;
	Relation	relation = node->ss.ss_currentRelation;
	EState	   *estate   = node->ss.ps.state;
	Snapshot	snapshot = estate->es_snapshot;
	int			num_rels = list_length(node->custom_ps);
	int			num_devs = 0;
	Size		len = 0;

	if (pts->br_state)
		len += pgstromBrinIndexEstimateDSM(pts);
	len += MAXALIGN(offsetof(pgstromSharedState, inners[num_rels]));

	if ((pts->xpu_task_flags & DEVKIND__NVIDIA_GPU) != 0)
		num_devs = numGpuDevAttrs;
	else if ((pts->xpu_task_flags & DEVKIND__NVIDIA_DPU) != 0)
		num_devs = DpuStorageEntryCount();
	len += MAXALIGN(sizeof(pg_atomic_uint32) * num_devs);

	if (!pts->arrow_state)
		len += table_parallelscan_estimate(relation, snapshot);

	return MAXALIGN(len);
}

/*
 * pgstromSharedStateInitDSM
 */
void
pgstromSharedStateInitDSM(CustomScanState *node,
						  ParallelContext *pcxt,
						  void *coordinate)
{
	pgstromTaskState *pts = (pgstromTaskState *)node;
	Relation	relation = node->ss.ss_currentRelation;
	EState	   *estate   = node->ss.ps.state;
	Snapshot	snapshot = estate->es_snapshot;
	int			num_rels = list_length(node->custom_ps);
	int			num_devs = 0;
	size_t		dsm_length = offsetof(pgstromSharedState, inners[num_rels]);
	char	   *dsm_addr = coordinate;
	pgstromSharedState *ps_state;
	TableScanDesc scan = NULL;

	Assert(!IsBackgroundWorker);
	if ((pts->xpu_task_flags & DEVKIND__NVIDIA_GPU) != 0)
		num_devs = numGpuDevAttrs;
	else if ((pts->xpu_task_flags & DEVKIND__NVIDIA_DPU) != 0)
		num_devs = DpuStorageEntryCount();

	if (pts->br_state)
		dsm_addr += pgstromBrinIndexInitDSM(pts, dsm_addr);
	Assert(!pts->css.ss.ss_currentScanDesc);
	if (dsm_addr)
	{
		ps_state = (pgstromSharedState *) dsm_addr;
		memset(ps_state, 0, dsm_length);
		ps_state->ss_handle = dsm_segment_handle(pcxt->seg);
		ps_state->ss_length = dsm_length;
		dsm_addr += MAXALIGN(dsm_length);

		/* control variables for parallel tasks */
		pts->rjoin_devs_count  = (pg_atomic_uint32 *)dsm_addr;
		memset(dsm_addr, 0, sizeof(pg_atomic_uint32) * num_devs);
		dsm_addr += MAXALIGN(sizeof(pg_atomic_uint32) * num_devs);
		pts->rjoin_exit_count = &ps_state->__rjoin_exit_count;

		/* parallel scan descriptor */
		if (pts->gcache_desc)
			pgstromGpuCacheInitDSM(pts, ps_state);
		if (pts->arrow_state)
			pgstromArrowFdwInitDSM(pts->arrow_state, ps_state);
		else
		{
			ParallelTableScanDesc pdesc = (ParallelTableScanDesc) dsm_addr;

			table_parallelscan_initialize(relation, pdesc, snapshot);
			scan = table_beginscan_parallel(relation, pdesc);
			ps_state->parallel_scan_desc_offset = ((char *)pdesc - (char *)ps_state);
		}
	}
	else
	{
		ps_state = MemoryContextAllocZero(estate->es_query_cxt, dsm_length);
		ps_state->ss_handle = DSM_HANDLE_INVALID;
		ps_state->ss_length = dsm_length;

		/* control variables for scan/rjoin */
		pts->rjoin_devs_count = (pg_atomic_uint32 *)
			MemoryContextAllocZero(estate->es_query_cxt,
								   sizeof(pg_atomic_uint32 *) * num_devs);
		pts->rjoin_exit_count = &ps_state->__rjoin_exit_count;

		/* scan descriptor */
		if (pts->gcache_desc)
			pgstromGpuCacheInitDSM(pts, ps_state);
		if (pts->arrow_state)
			pgstromArrowFdwInitDSM(pts->arrow_state, ps_state);
		else
			scan = table_beginscan(relation, estate->es_snapshot, 0, NULL);
	}
	ps_state->query_plan_id = ((uint64_t)MyProcPid) << 32 |
		(uint64_t)pts->css.ss.ps.plan->plan_node_id;	
	ps_state->num_rels = num_rels;
	ConditionVariableInit(&ps_state->preload_cond);
	SpinLockInit(&ps_state->preload_mutex);
	if (num_rels > 0)
		ps_state->preload_shmem_handle = __shmemCreate(pts->ds_entry);
	pts->ps_state = ps_state;
	pts->css.ss.ss_currentScanDesc = scan;
}

/*
 * pgstromSharedStateAttachDSM
 */
void
pgstromSharedStateAttachDSM(CustomScanState *node,
							shm_toc *toc,
							void *coordinate)
{
	pgstromTaskState *pts = (pgstromTaskState *)node;
	pgstromSharedState *ps_state;
	char	   *dsm_addr = coordinate;
	int			num_rels = list_length(pts->css.custom_ps);
	int			num_devs = 0;

	if ((pts->xpu_task_flags & DEVKIND__NVIDIA_GPU) != 0)
		num_devs = numGpuDevAttrs;
	else if ((pts->xpu_task_flags & DEVKIND__NVIDIA_DPU) != 0)
		num_devs = DpuStorageEntryCount();

	if (pts->br_state)
		dsm_addr += pgstromBrinIndexAttachDSM(pts, dsm_addr);
	pts->ps_state = ps_state = (pgstromSharedState *)dsm_addr;
	Assert(ps_state->num_rels == num_rels);
	dsm_addr += MAXALIGN(offsetof(pgstromSharedState, inners[num_rels]));

	pts->rjoin_exit_count = &ps_state->__rjoin_exit_count;
	pts->rjoin_devs_count = (pg_atomic_uint32 *)dsm_addr;
	dsm_addr += MAXALIGN(sizeof(pg_atomic_uint32) * num_devs);

	if (pts->gcache_desc)
		pgstromGpuCacheAttachDSM(pts, pts->ps_state);
	if (pts->arrow_state)
		pgstromArrowFdwAttachDSM(pts->arrow_state, pts->ps_state);
	else
	{
		Relation	relation = pts->css.ss.ss_currentRelation;
		ParallelTableScanDesc pdesc = (ParallelTableScanDesc) dsm_addr;

		pts->css.ss.ss_currentScanDesc = table_beginscan_parallel(relation, pdesc);
	}
}

/*
 * pgstromSharedStateShutdownDSM
 */
void
pgstromSharedStateShutdownDSM(CustomScanState *node)
{
	pgstromTaskState   *pts = (pgstromTaskState *) node;
	pgstromSharedState *src_state = pts->ps_state;
	pgstromSharedState *dst_state;
	EState			   *estate = node->ss.ps.state;

	if (pts->br_state)
		pgstromBrinIndexShutdownDSM(pts);
	if (pts->gcache_desc)
		pgstromGpuCacheShutdown(pts);
	if (pts->arrow_state)
		pgstromArrowFdwShutdown(pts->arrow_state);
	if (src_state)
	{
		size_t	sz = offsetof(pgstromSharedState,
							  inners[src_state->num_rels]);
		dst_state = MemoryContextAllocZero(estate->es_query_cxt, sz);
		memcpy(dst_state, src_state, sz);
		pts->ps_state = dst_state;
	}
}

/*
 * pgstromGpuDirectExplain
 */
static void
pgstromGpuDirectExplain(pgstromTaskState *pts,
						ExplainState *es, List *dcontext)
{
	pgstromSharedState *ps_state = pts->ps_state;
	StringInfoData		buf;

	initStringInfo(&buf);
	if (!es->analyze || !ps_state)
	{
		if (bms_is_empty(pts->optimal_gpus))
		{
			appendStringInfo(&buf, "disabled");
		}
		else if (pgstrom_regression_test_mode)
		{
			appendStringInfo(&buf, "enabled");
		}
		else
		{
			bool	is_first = true;
			int		k;

			appendStringInfo(&buf, "enabled (");
			for (k = bms_next_member(pts->optimal_gpus, -1);
				 k >= 0;
				 k = bms_next_member(pts->optimal_gpus, k))
			{
				if (!is_first)
					appendStringInfo(&buf, ", ");
				appendStringInfo(&buf, "GPU-%d", k);
				is_first = false;
			}
			appendStringInfo(&buf, ")");
		}
	}
	else
	{
		XpuConnection  *conn = pts->conn;
		uint64			count;
		int				pos;

		appendStringInfo(&buf, "%s (", (bms_is_empty(pts->optimal_gpus)
										? "disabled"
										: "enabled"));
		if (!pgstrom_regression_test_mode && conn)
			appendStringInfo(&buf, "%s; ", conn->devname);
		pos = buf.len;

		count = pg_atomic_read_u64(&ps_state->npages_buffer_read);
		if (count)
			appendStringInfo(&buf, "%sbuffer=%lu",
							 (buf.len > pos ? ", " : ""),
							 count / PAGES_PER_BLOCK);
		count = pg_atomic_read_u64(&ps_state->npages_vfs_read);
		if (count)
			appendStringInfo(&buf, "%svfs=%lu",
							 (buf.len > pos ? ", " : ""),
							 count / PAGES_PER_BLOCK);
		count = pg_atomic_read_u64(&ps_state->npages_direct_read);
		if (count)
			appendStringInfo(&buf, "%sdirect=%lu",
							 (buf.len > pos ? ", " : ""),
							 count / PAGES_PER_BLOCK);
		count = pg_atomic_read_u64(&ps_state->source_ntuples_raw);
		appendStringInfo(&buf, "%sntuples=%lu",
						 (buf.len > pos ? ", " : ""),
						 count);
		appendStringInfo(&buf, ")");
	}
	ExplainPropertyText("GPU-Direct SQL", buf.data, es);

	pfree(buf.data);
}

/*
 * pgstromExplainTaskState
 */
void
pgstromExplainTaskState(CustomScanState *node,
						List *ancestors,
						ExplainState *es)
{
	pgstromTaskState   *pts = (pgstromTaskState *)node;
	pgstromSharedState *ps_state = pts->ps_state;
	pgstromPlanInfo	   *pp_info = pts->pp_info;
	CustomScan		   *cscan = (CustomScan *)node->ss.ps.plan;
	bool				verbose = (cscan->custom_plans != NIL);
	List			   *dcontext;
	StringInfoData		buf;
	ListCell		   *lc;
	const char		   *xpu_label;
	char				label[100];
	char			   *str;
	double				ntuples;
	uint64_t			stat_ntuples = 0;

	/* setup deparse context */
	dcontext = set_deparse_context_plan(es->deparse_cxt,
										node->ss.ps.plan,
										ancestors);
	if ((pts->xpu_task_flags & DEVKIND__NVIDIA_GPU) != 0)
		xpu_label = "GPU";
	else if ((pts->xpu_task_flags & DEVKIND__NVIDIA_DPU) != 0)
		xpu_label = "DPU";
	else
		xpu_label = "???";

	/* xPU Projection */
	initStringInfo(&buf);
	foreach (lc, cscan->custom_scan_tlist)
	{
		TargetEntry *tle = lfirst(lc);

		if (tle->resjunk)
			continue;
		str = deparse_expression((Node *)tle->expr, dcontext, verbose, true);
		if (buf.len > 0)
			appendStringInfoString(&buf, ", ");
		appendStringInfoString(&buf, str);
	}
	snprintf(label, sizeof(label),
			 "%s Projection", xpu_label);
	ExplainPropertyText(label, buf.data, es);

	/* xPU Scan Quals */
	if (ps_state)
		stat_ntuples = pg_atomic_read_u64(&ps_state->source_ntuples_in);
	if (pp_info->scan_quals_explain)
	{
		List   *scan_quals = pp_info->scan_quals_explain;
		Expr   *expr;

		resetStringInfo(&buf);
		if (list_length(scan_quals) > 1)
			expr = make_andclause(scan_quals);
		else
			expr = linitial(scan_quals);
		str = deparse_expression((Node *)expr, dcontext, verbose, true);
		appendStringInfoString(&buf, str);
		if (!es->analyze)
		{
			appendStringInfo(&buf, " [rows: %.0f -> %.0f]",
							 pp_info->scan_tuples,
							 pp_info->scan_nrows);
		}
		else
		{
			uint64_t		prev_ntuples = 0;

			if (ps_state)
				prev_ntuples = pg_atomic_read_u64(&ps_state->source_ntuples_raw);
			appendStringInfo(&buf, " [plan: %.0f -> %.0f, exec: %lu -> %lu]",
							 pp_info->scan_tuples,
							 pp_info->scan_nrows,
							 prev_ntuples,
							 stat_ntuples);
		}
		snprintf(label, sizeof(label), "%s Scan Quals", xpu_label);
		ExplainPropertyText(label, buf.data, es);
	}

	/* xPU JOIN */
	ntuples = pp_info->scan_nrows;
	for (int i=0; i < pp_info->num_rels; i++)
	{
		pgstromPlanInnerInfo *pp_inner = &pp_info->inners[i];

		if (pp_inner->join_quals_original != NIL ||
			pp_inner->other_quals_original != NIL)
		{
			const char *join_label;

			resetStringInfo(&buf);
			foreach (lc, pp_inner->join_quals_original)
			{
				Node   *expr = lfirst(lc);

				str = deparse_expression(expr, dcontext, verbose, true);
				if (buf.len > 0)
					appendStringInfoString(&buf, ", ");
				appendStringInfoString(&buf, str);
			}
			if (pp_inner->other_quals_original != NIL)
			{
				foreach (lc, pp_inner->other_quals_original)
				{
					Node   *expr = lfirst(lc);

					str = deparse_expression(expr, dcontext, verbose, true);
					if (buf.len > 0)
						appendStringInfoString(&buf, ", ");
					appendStringInfo(&buf, "[%s]", str);
				}
			}
			if (!es->analyze || !ps_state)
			{
				appendStringInfo(&buf, " ... [nrows: %.0f -> %.0f]",
								 ntuples, pp_inner->join_nrows);
			}
			else
			{
				uint64_t	next_ntuples;

				next_ntuples = pg_atomic_read_u64(&ps_state->inners[i].stats_join);
				appendStringInfo(&buf, " ... [plan: %.0f -> %.0f, exec: %lu -> %lu]",
								 ntuples, pp_inner->join_nrows,
								 stat_ntuples,
								 next_ntuples);
				stat_ntuples = next_ntuples;
			}
			switch (pp_inner->join_type)
			{
				case JOIN_INNER: join_label = "Join"; break;
				case JOIN_LEFT:  join_label = "Left Outer Join"; break;
				case JOIN_RIGHT: join_label = "Right Outer Join"; break;
				case JOIN_FULL:  join_label = "Full Outer Join"; break;
				case JOIN_SEMI:  join_label = "Semi Join"; break;
				case JOIN_ANTI:  join_label = "Anti Join"; break;
				default:         join_label = "??? Join"; break;
			}
			snprintf(label, sizeof(label),
					 "%s %s Quals [%d]", xpu_label, join_label, i+1);
			ExplainPropertyText(label, buf.data, es);
		}
		ntuples = pp_inner->join_nrows;

		if (pp_inner->hash_outer_keys_original != NIL)
		{
			resetStringInfo(&buf);
			foreach (lc, pp_inner->hash_outer_keys_original)
			{
				Node   *expr = lfirst(lc);

				str = deparse_expression(expr, dcontext, verbose, true);
				if (buf.len > 0)
					appendStringInfoString(&buf, ", ");
				appendStringInfoString(&buf, str);
			}
			snprintf(label, sizeof(label),
					 "%s Outer Hash [%d]", xpu_label, i+1);
			ExplainPropertyText(label, buf.data, es);
		}
		if (pp_inner->hash_inner_keys_original != NIL)
		{
			resetStringInfo(&buf);
			foreach (lc, pp_inner->hash_inner_keys_original)
			{
				Node   *expr = lfirst(lc);

				str = deparse_expression(expr, dcontext, verbose, true);
				if (buf.len > 0)
					appendStringInfoString(&buf, ", ");
				appendStringInfoString(&buf, str);
			}
			snprintf(label, sizeof(label),
					 "%s Inner Hash [%d]", xpu_label, i+1);
			ExplainPropertyText(label, buf.data, es);
		}
		if (pp_inner->gist_clause)
		{
			char   *idxname = get_rel_name(pp_inner->gist_index_oid);
			char   *colname = get_attname(pp_inner->gist_index_oid,
										  pp_inner->gist_index_col, false);
			resetStringInfo(&buf);

			str = deparse_expression((Node *)pp_inner->gist_clause,
									 dcontext, verbose, true);
			appendStringInfoString(&buf, str);
			if (idxname && colname)
				appendStringInfo(&buf, " on %s (%s)", idxname, colname);
			if (es->analyze && ps_state)
			{
				appendStringInfo(&buf, " [fetched: %lu]",
								 pg_atomic_read_u64(&ps_state->inners[i].stats_gist));
			}
			snprintf(label, sizeof(label),
					 "%s GiST Join [%d]", xpu_label, i+1);
			ExplainPropertyText(label, buf.data, es);
		}
	}
	if (pp_info->sibling_param_id >= 0)
		ExplainPropertyInteger("Inner Siblings-Id", NULL,
							   pp_info->sibling_param_id, es);

	/*
	 * Storage related info
	 */
	if (pts->arrow_state)
	{
		pgstromArrowFdwExplain(pts->arrow_state,
							   pts->css.ss.ss_currentRelation,
							   es, dcontext);
		pgstromGpuDirectExplain(pts, es, dcontext);
	}
	else if (pts->gcache_desc)
	{
		/* GPU-Cache */
		pgstromGpuCacheExplain(pts, es, dcontext);
	}
	else if (!bms_is_empty(pts->optimal_gpus))
	{
		/* GPU-Direct */
		pgstromGpuDirectExplain(pts, es, dcontext);
	}
	else if (pts->ds_entry)
	{
		/* DPU-Entry */
		explainDpuStorageEntry(pts->ds_entry, es);
	}
	else
	{
		/* Normal Heap Storage */
	}
	/* State of BRIN-index */
	if (pts->br_state)
		pgstromBrinIndexExplain(pts, dcontext, es);

	/*
	 * Dump the XPU code (only if verbose)
	 */
	if (es->verbose)
	{
		pgstrom_explain_kvars_slot(&pts->css, es, dcontext);
		pgstrom_explain_kvecs_buffer(&pts->css, es, dcontext);
		pgstrom_explain_xpucode(&pts->css, es, dcontext,
								"LoadVars OpCode",
								pp_info->kexp_load_vars_packed);
		pgstrom_explain_xpucode(&pts->css, es, dcontext,
								"MoveVars OpCode",
								pp_info->kexp_move_vars_packed);
		pgstrom_explain_xpucode(&pts->css, es, dcontext,
								"Scan Quals OpCode",
								pp_info->kexp_scan_quals);
		pgstrom_explain_xpucode(&pts->css, es, dcontext,
								"Join Quals OpCode",
								pp_info->kexp_join_quals_packed);
		pgstrom_explain_xpucode(&pts->css, es, dcontext,
								"Join HashValue OpCode",
								pp_info->kexp_hash_keys_packed);
		pgstrom_explain_xpucode(&pts->css, es, dcontext,
								"GiST-Index Join OpCode",
								pp_info->kexp_gist_evals_packed);
		pgstrom_explain_xpucode(&pts->css, es, dcontext,
								"Projection OpCode",
								pp_info->kexp_projection);
		pgstrom_explain_xpucode(&pts->css, es, dcontext,
								"Group-By KeyHash OpCode",
								pp_info->kexp_groupby_keyhash);
		pgstrom_explain_xpucode(&pts->css, es, dcontext,
								"Group-By KeyLoad OpCode",
								pp_info->kexp_groupby_keyload);
		pgstrom_explain_xpucode(&pts->css, es, dcontext,
								"Group-By KeyComp OpCode",
								pp_info->kexp_groupby_keycomp);
		pgstrom_explain_xpucode(&pts->css, es, dcontext,
								"Partial Aggregation OpCode",
								pp_info->kexp_groupby_actions);
		if (pp_info->groupby_prepfn_bufsz > 0)
			ExplainPropertyInteger("Partial Function BufSz", NULL,
								   pp_info->groupby_prepfn_bufsz, es);
		if (pp_info->cuda_stack_size > 0)
			ExplainPropertyInteger("CUDA Stack Size", NULL,
								   pp_info->cuda_stack_size, es);
	}
	pfree(buf.data);
}

/*
 * __xpuClientOpenSession
 */
void
__xpuClientOpenSession(pgstromTaskState *pts, // QQQ 后端
					   const XpuCommand *session,
					   pgsocket sockfd,
					   const char *devname,
					   int dev_index)
{
	XpuConnection  *conn;
	XpuCommand	   *resp;
	int				rv;

	Assert(!pts->conn);
	conn = calloc(1, sizeof(XpuConnection));
	if (!conn)
	{
		close(sockfd);
		elog(ERROR, "out of memory");
	}
	strncpy(conn->devname, devname, 32);
	conn->dev_index = dev_index;
	conn->sockfd = sockfd;
	conn->resowner = CurrentResourceOwner;
	conn->worker = pthread_self();	/* to be over-written by worker's-id */
	pthreadMutexInit(&conn->mutex);
	conn->num_running_cmds = 0;
	conn->num_ready_cmds = 0;
	dlist_init(&conn->ready_cmds_list);
	dlist_init(&conn->active_cmds_list);
	dlist_push_tail(&xpu_connections_list, &conn->chain);
	pts->conn = conn;

	/*
	 * Ok, sockfd and conn shall be automatically released on ereport()
	 * after that.
	 */
	if ((rv = pthread_create(&conn->worker, NULL,
							 __xpuConnectSessionWorker, conn)) != 0) // QQQ 线程
		elog(ERROR, "failed on pthread_create: %s", strerror(rv));

	/*
	 * Initialize the new session
	 */
	Assert(session->tag == XpuCommandTag__OpenSession);
	xpuClientSendCommand(conn, session);
	resp = __waitAndFetchNextXpuCommand(pts, false);
	if (!resp)
		elog(ERROR, "Bug? %s:OpenSession response is missing", conn->devname);
	if (resp->tag != XpuCommandTag__Success)
		elog(ERROR, "%s:OpenSession failed - %s (%s:%d %s)",
			 conn->devname,
			 resp->u.error.message,
			 resp->u.error.filename,
			 resp->u.error.lineno,
			 resp->u.error.funcname);
	xpuClientPutResponse(resp);
}

/*
 * pgstrom_init_executor
 */
void
pgstrom_init_executor(void)
{
	dlist_init(&xpu_connections_list);
	RegisterResourceReleaseCallback(xpuclientCleanupConnections, NULL);
}
