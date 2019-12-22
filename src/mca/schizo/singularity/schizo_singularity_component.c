/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2016-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2019      Sylabs, Inc. All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prrte_config.h"
#include "src/include/types.h"

#include "src/util/show_help.h"

#include "src/runtime/prrte_globals.h"

#include "src/mca/schizo/schizo.h"
#include "schizo_singularity.h"

static int component_query(prrte_mca_base_module_t **module, int *priority);

/*
 * Struct of function pointers and all that to let us be initialized
 */
prrte_schizo_base_component_t prrte_schizo_singularity_component = {
    .base_version = {
        PRRTE_MCA_SCHIZO_BASE_VERSION_1_0_0,
        .mca_component_name = "singularity",
        PRRTE_MCA_BASE_MAKE_VERSION(component, PRRTE_MAJOR_VERSION, PRRTE_MINOR_VERSION,
                              PRRTE_RELEASE_VERSION),
        .mca_query_component = component_query,
    },
    .base_data = {
        /* The component is checkpoint ready */
        PRRTE_MCA_BASE_METADATA_PARAM_CHECKPOINT
    },
};

static int component_query(prrte_mca_base_module_t **module, int *priority)
{
    *module = (prrte_mca_base_module_t*)&prrte_schizo_singularity_module;
    *priority = 9;
    return PRRTE_SUCCESS;
}

