/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2022-2022. All rights reserved.
 */

#include "gatherv.h"
#include "planc_hccl_dt.h"

#include "util/ucg_helper.h"
#include "util/algo/ucg_kntree.h"

#define UCG_PLANC_HCCL_GATHERV_CHECK_GOTO(_status, _label) \
    if (_status != HCCL_SUCCESS) { \
        goto _label; \
    }

#define UCG_PLANC_HCCL_GATHERV_CHECK_ACL(_acl_func) \
    ({ \
        HcclResult status = HCCL_SUCCESS; \
        aclError rc = _acl_func; \
        if (rc != ACL_SUCCESS) { \
            status = HCCL_E_PARA; \
            ucg_error("ACL error %s", aclGetRecentErrMsg()); \
        } \
        status; \
    })

#define UCG_PLANC_HCCL_GATHERV_CHECK_HCCL(_hccl_func) \
    ({ \
        HcclResult rc = _hccl_func; \
        if (rc != HCCL_SUCCESS) { \
            ucg_error("HCCL error %s\n ACL error %s", \
                      HcclResultString(rc), aclGetRecentErrMsg()); \
        } \
        rc; \
    })

#define UCG_PLANC_HCCL_GATHERV_CHECK_ACL_GOTO(_acl_func, _status, _label) \
    do { \
        _status = UCG_PLANC_HCCL_GATHERV_CHECK_ACL(_acl_func); \
        UCG_PLANC_HCCL_GATHERV_CHECK_GOTO(_status, _label); \
    } while (0)

#define UCG_PLANC_HCCL_GATHERV_CHECK_HCCL_GOTO(_hccl_func, _status, _label) \
    do { \
        _status = UCG_PLANC_HCCL_GATHERV_CHECK_HCCL(_hccl_func); \
        UCG_PLANC_HCCL_GATHERV_CHECK_GOTO(_status, _label); \
    } while (0)    


/* Function encapsulated to ensure that trigger modes are consistent. */
static HcclResult Hccl_gatherv_kntree(const void *sendbuf, const int32_t sendcount,
                                      HcclDataType sendtype,
                                      void *recvbuf, 
                                      const int32_t *recvcounts,
                                      const int32_t *displs, 
                                      HcclDataType recvtype, int32_t root,
                                      ucg_planc_hccl_op_t *op)
{
    
    ucg_planc_hccl_group_t *hccl_group = op->hccl_group;
    ucg_rank_t myrank = UCG_PLANC_HCCL_GROUP_MYRANK(hccl_group);
    ucg_rank_t size = UCG_PLANC_HCCL_GROUP_SIZE(hccl_group);    
    ucg_oob_group_t *oob_group = UCG_PLANC_HCCL_GROUP_OOB(hccl_group);
    ucg_assert(oob_group->size == size);
    size_t recvtype_ext = ucg_planc_hccl_dt_size(recvtype);
    HcclResult err;

    int64_t len = size * 2 * sizeof(int32_t);
    int32_t *recvcounts_temp_s = (int32_t *)malloc(len);
    int32_t *recvcounts_temp_r = (int32_t *)malloc(len * size);
    if (myrank == root) {
        memcpy(recvcounts_temp_s, recvcounts, size * sizeof(int32_t));
        memcpy(recvcounts_temp_s + size, displs, size * sizeof(int32_t));
    }
    ucg_status_t status = oob_group->allgather(recvcounts_temp_s, recvcounts_temp_r,
                                               len, oob_group->group);
    if (status != UCG_OK) {
        err = HCCL_E_PARA;
        goto free_recvcounts_temp;
    }
    int32_t *recvcounts_temp = &recvcounts_temp_r[0];

    /* create staging area */
    ucg_algo_kntree_iter_t iter;
    ucg_algo_kntree_iter_init(&iter, size, 2, root, myrank, 1);

    int64_t *staging_displs = NULL;
    void *staging_area_dev = NULL;
    if ((myrank != root)) {
        int64_t staging_len     = 0;
        int32_t staging_count   = ucg_algo_kntree_get_subtree_size(&iter, myrank) - 1;

        if (staging_count > 0) {
            staging_displs = (int64_t *)malloc(staging_count * sizeof(int64_t));

            for (int32_t i = 0; i < staging_count; i++) {
                int32_t idx = (myrank + i + 1) % size;
                staging_displs[i] = staging_len;
                staging_len += recvcounts_temp[idx] * recvtype_ext;
            }
            
            if (staging_len > 0) {
                if (op->dev_stage_area == NULL) {
                    UCG_PLANC_HCCL_GATHERV_CHECK_ACL_GOTO(
                        aclrtMalloc(&op->dev_stage_area, staging_len, ACL_MEM_MALLOC_HUGE_FIRST),
                        err,
                        free_staging_area
                    );
                }
                staging_area_dev = op->dev_stage_area;
            }
        }
    }

    /* recv from child */
    ucg_rank_t recv_peer;
    while ((recv_peer = ucg_algo_kntree_iter_child_value(&iter)) != UCG_INVALID_RANK) {
        if (myrank == root) {
            int32_t children_size = ucg_algo_kntree_get_subtree_size(&iter, recv_peer);
            for (int32_t i = 0; i < children_size; i++) {
                int32_t idx = (recv_peer + i) % size;
                int64_t offset = displs[idx] * recvtype_ext;
                if (recvcounts[idx] > 0) {
                    UCG_PLANC_HCCL_GATHERV_CHECK_HCCL_GOTO(
                        HcclRecv(recvbuf + offset, recvcounts[idx], recvtype, recv_peer, 
                               hccl_group->comm, hccl_group->stream),
                        err,
                        free_staging_area
                    );
                }
            }
        } else {
            int32_t staging_count = ucg_algo_kntree_get_subtree_size(&iter, recv_peer);
            if (staging_count > 0) {
                for (int32_t i = 0; i < staging_count; i++) {
                    int32_t idx = (i + recv_peer) % size;
                    int32_t staging_displs_idx = (myrank < idx) ?
                                                 (idx - myrank - 1) :
                                                 (idx + size - myrank - 1);                    
                    if (recvcounts_temp[idx] > 0) {
                        UCG_PLANC_HCCL_GATHERV_CHECK_HCCL_GOTO(
                            HcclRecv(staging_area_dev + staging_displs[staging_displs_idx], recvcounts_temp[idx], 
                                     recvtype, recv_peer, hccl_group->comm, hccl_group->stream),
                            err,
                            free_staging_area
                        );
                    }
                }
            }
        }
        ucg_algo_kntree_iter_child_inc(&iter);
    }

    /* send to parent */
    ucg_rank_t send_peer;
    send_peer = ucg_algo_kntree_iter_parent_value(&iter);
    if (send_peer == UCG_INVALID_RANK) {
        ucg_assert(myrank == root);
        if (sendbuf != UCG_IN_PLACE) {
            UCG_PLANC_HCCL_GATHERV_CHECK_ACL_GOTO(
                aclrtMemcpyAsync(recvbuf + displs[myrank] * recvtype_ext, 
                                 recvcounts[myrank] * recvtype_ext,
                                 sendbuf,
                                 recvcounts[myrank] * recvtype_ext,
                                 ACL_MEMCPY_DEVICE_TO_DEVICE, hccl_group->stream),
                err,
                free_staging_area
            );
        }
    } else {
        if (sendcount > 0) {
            UCG_PLANC_HCCL_GATHERV_CHECK_HCCL_GOTO(
                HcclSend((void *)sendbuf, sendcount, sendtype, send_peer, 
                          hccl_group->comm, hccl_group->stream),
                err,
                free_staging_area
            );
        }
            
        int64_t staging_len     = 0;
        int32_t staging_count   = ucg_algo_kntree_get_subtree_size(&iter, myrank) - 1;
        if (staging_count > 0) {
            for (int32_t i = 0; i < staging_count; i++) {
                int32_t idx = (myrank + i + 1) % size;
                int32_t send_len = recvcounts_temp[idx] * recvtype_ext;
                if (recvcounts_temp[idx] > 0) {
                    UCG_PLANC_HCCL_GATHERV_CHECK_HCCL_GOTO(
                        HcclSend(staging_area_dev + staging_len, recvcounts_temp[idx] , recvtype, send_peer, 
                                 hccl_group->comm, hccl_group->stream),
                        err,
                        free_staging_area
                    );
                }
                staging_len += send_len;
            }
        }
    }

free_staging_area:
    if (staging_displs != NULL) {
        free(staging_displs);
        staging_displs = NULL;
    }

free_recvcounts_temp:
    free(recvcounts_temp_s); recvcounts_temp_s = NULL;
    free(recvcounts_temp_r); recvcounts_temp_r = NULL;    

    return err;
}

static ucg_status_t ucg_planc_hccl_gatherv_kntree_op_trigger(ucg_plan_op_t *ucg_op)
{
    ucg_status_t status = UCG_OK;
    const ucg_coll_gatherv_args_t *coll_args = &ucg_op->super.args.gatherv;
    ucg_planc_hccl_op_t *op = ucg_derived_of(ucg_op, ucg_planc_hccl_op_t);

    UCG_PLANC_HCCL_OP_TRIGGER(
        Hccl_gatherv_kntree(coll_args->sendbuf, coll_args->sendcount,
                            ucg_planc_hccl_dt_u2h(coll_args->sendtype),
                            coll_args->recvbuf, 
                            coll_args->recvcounts,
                            coll_args->displs,
                            ucg_planc_hccl_dt_u2h(coll_args->recvtype),
                            coll_args->root,
                            op),
        op, status);
    return status;
}

ucg_status_t ucg_planc_hccl_gatherv_kntree_prepare(ucg_vgroup_t *vgroup,
                                                   const ucg_coll_args_t *args,
                                                   ucg_plan_op_t **op)
{
    UCG_CHECK_NULL_INVALID(vgroup, args, op);

    const ucg_coll_gatherv_args_t *coll_args = &args->gatherv;
    if (!ucg_planc_hccl_dt_is_supported(coll_args->sendtype) ||
        !ucg_planc_hccl_dt_is_supported(coll_args->recvtype)) {
        return UCG_ERR_UNSUPPORTED;
    }

    ucg_planc_hccl_group_t *hccl_group = ucg_derived_of(vgroup, ucg_planc_hccl_group_t);
    ucg_planc_hccl_op_t *hccl_op = ucg_mpool_get(&hccl_group->context->op_mp);
    if (hccl_op == NULL) {
        return UCG_ERR_NO_MEMORY;
    }

    ucg_status_t status;
    status = UCG_CLASS_CONSTRUCT(ucg_plan_op_t, &hccl_op->super, vgroup,
                                 ucg_planc_hccl_gatherv_kntree_op_trigger,
                                 ucg_planc_hccl_op_progress,
                                 ucg_planc_hccl_op_discard,
                                 args);
    if (status != UCG_OK) {
        ucg_error("Failed to initialize super of hccl op");
        ucg_mpool_put(hccl_op);
        return status;
    }

    hccl_op->dev_stage_area = NULL;
    hccl_op->hccl_group = hccl_group;

    *op = &hccl_op->super;
    return UCG_OK;
}