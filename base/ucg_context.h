/*
 * Copyright (C) Huawei Technologies Co., Ltd. 2019.  ALL RIGHTS RESERVED.
 * See file LICENSE for terms.
 */

#ifndef UCG_CONTEXT_H_
#define UCG_CONTEXT_H_

#include <ucs/config/types.h>
#include <ucp/core/ucp_context.h>

/* Note: <ucs/api/...> not used because this header is not installed */
#include "../api/ucg_plan_component.h"

typedef struct ucg_config {
    /** Array of planner names to use */
    ucs_config_names_array_t planners;

    /** Up to how many operations should be cached in each group */
    unsigned group_cache_size_thresh;

    /** Above how many group members should UCG preconnect all the topologies */
    unsigned group_preconnect_thresh;

    /** Above how many group members should UCG initiate collective transports */
    unsigned coll_iface_member_thresh;

    /** Should datatypes be treated as volatile and reloaded on each invocation */
    int is_volatile_datatype;

    /** Used for passing UCP configuration (not set by @ref ucg_read_config ) */
    ucp_config_t *ucp_config;
} ucg_context_config_t;

/*
 * To enable the "Groups" feature in UCX - it's registered as part of the UCX
 * context - and allocated a context slot in each UCP Worker at a certain offset.
 */
typedef struct ucg_context {
#if ENABLE_FAULT_TOLERANCE
    ucg_ft_ctx_t          ft_ctx; /* If supported - fault-tolerance context */
#endif

    ucg_plan_desc_t      *planners;
    void                 *planners_ctx;
    unsigned              num_planners;
    size_t                per_group_planners_ctx;
    ucs_list_link_t       groups_head;
    ucg_group_id_t        next_group_id;

    ucg_context_config_t  config;
    ucp_context_t         ucp_ctx; /* must be last, for ABI compatibility */
} ucg_context_t;

void ucg_context_copy_used_ucg_params(ucg_params_t *dst,
                                      const ucg_params_t *src);

#endif /* UCG_CONTEXT_H_ */
