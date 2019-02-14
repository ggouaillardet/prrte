/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2004-2010 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2008 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2006-2017 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2007-2009 Sun Microsystems, Inc. All rights reserved.
 * Copyright (c) 2007-2017 Los Alamos National Security, LLC.  All rights
 *                         reserved.
 * Copyright (c) 2013-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2015-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "orte_config.h"
#include "orte/constants.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#ifdef HAVE_STRINGS_H
#include <strings.h>
#endif  /* HAVE_STRINGS_H */
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifdef HAVE_SYS_PARAM_H
#include <sys/param.h>
#endif
#include <errno.h>
#include <ctype.h>
#ifdef HAVE_POLL_H
#include <poll.h>
#endif

#include "opal/event/event-internal.h"
#include "opal/pmix/pmix-internal.h"
#include "opal/mca/base/base.h"
#include "opal/util/argv.h"
#include "opal/util/output.h"
#include "opal/util/opal_environ.h"
#include "opal/util/show_help.h"

#include "opal/version.h"
#include "opal/runtime/opal.h"
#include "opal/runtime/opal_info_support.h"
#include "opal/runtime/opal_progress_threads.h"

#include "orte/runtime/runtime.h"
#include "orte/runtime/orte_globals.h"
#include "orte/mca/errmgr/errmgr.h"
#include "orte/mca/schizo/base/base.h"
#include "orte/orted/orted_submit.h"

/* ensure I can behave like a daemon */
#include "pterminate.h"

static struct {
    bool system_server_first;
    bool system_server_only;
    bool do_not_connect;
    bool wait_to_connect;
    int num_retries;
    int pid;
} myoptions;

typedef struct {
    opal_pmix_lock_t lock;
    pmix_info_t *info;
    size_t ninfo;
} mylock_t;

static opal_list_t job_info;
static orte_jobid_t myjobid = ORTE_JOBID_INVALID;

static size_t evid = INT_MAX;
static pmix_proc_t myproc;

static opal_cmd_line_init_t cmd_line_init[] = {
    /* look first for a system server */
    { NULL, '\0', "system-server-first", "system-server-first", 0,
      &myoptions.system_server_first, OPAL_CMD_LINE_TYPE_BOOL,
      "First look for a system server and connect to it if found", OPAL_CMD_LINE_OTYPE_DVM },

    /* connect only to a system server */
    { NULL, '\0', "system-server-only", "system-server-only", 0,
      &myoptions.system_server_only, OPAL_CMD_LINE_TYPE_BOOL,
      "Connect only to a system-level server", OPAL_CMD_LINE_OTYPE_DVM },

    /* do not connect */
    { NULL, '\0', "do-not-connect", "do-not-connect", 0,
      &myoptions.do_not_connect, OPAL_CMD_LINE_TYPE_BOOL,
      "Do not connect to a server", OPAL_CMD_LINE_OTYPE_DVM },

    /* wait to connect */
    { NULL, '\0', "wait-to-connect", "wait-to-connect", 0,
      &myoptions.wait_to_connect, OPAL_CMD_LINE_TYPE_INT,
      "Delay specified number of seconds before trying to connect", OPAL_CMD_LINE_OTYPE_DVM },

    /* number of times to try to connect */
    { NULL, '\0', "num-connect-retries", "num-connect-retries", 0,
      &myoptions.num_retries, OPAL_CMD_LINE_TYPE_INT,
      "Max number of times to try to connect", OPAL_CMD_LINE_OTYPE_DVM },

    /* provide a connection PID */
    { NULL, '\0', "pid", "pid", 1,
      &myoptions.pid, OPAL_CMD_LINE_TYPE_INT,
      "PID of the session-level daemon to which we should connect",
      OPAL_CMD_LINE_OTYPE_DVM },

    /* End of list */
    { NULL, '\0', NULL, NULL, 0,
      NULL, OPAL_CMD_LINE_TYPE_NULL, NULL }
};


static void infocb(pmix_status_t status,
                   pmix_info_t *info, size_t ninfo,
                   void *cbdata,
                   pmix_release_cbfunc_t release_fn,
                   void *release_cbdata)
{
    opal_pmix_lock_t *lock = (opal_pmix_lock_t*)cbdata;
#if PMIX_VERSION_MAJOR == 3 && PMIX_VERSION_MINOR == 0 && PMIX_VERSION_RELEASE < 3
    /* The callback should likely not have been called
     * see the comment below */
    if (PMIX_ERR_COMM_FAILURE == status) {
        return;
    }
#endif
    OPAL_ACQUIRE_OBJECT(lock);

    if (orte_cmd_options.verbose) {
        opal_output(0, "PRUN: INFOCB");
    }

    if (NULL != release_fn) {
        release_fn(release_cbdata);
    }
    OPAL_PMIX_WAKEUP_THREAD(lock);
}

static void regcbfunc(pmix_status_t status, size_t ref, void *cbdata)
{
    opal_pmix_lock_t *lock = (opal_pmix_lock_t*)cbdata;
    OPAL_ACQUIRE_OBJECT(lock);
    evid = ref;
    OPAL_PMIX_WAKEUP_THREAD(lock);
}

static void evhandler(size_t evhdlr_registration_id,
                      pmix_status_t status,
                      const pmix_proc_t *source,
                      pmix_info_t info[], size_t ninfo,
                      pmix_info_t *results, size_t nresults,
                      pmix_event_notification_cbfunc_fn_t cbfunc,
                      void *cbdata)
{
    opal_pmix_lock_t *lock = NULL;
    int jobstatus=0, rc;
    orte_jobid_t jobid = ORTE_JOBID_INVALID;
    size_t n;
    char *msg = NULL;

    if (orte_cmd_options.verbose) {
        opal_output(0, "PRUN: EVHANDLER WITH STATUS %s(%d)", PMIx_Error_string(status), status);
    }

    /* we should always have info returned to us - if not, there is
     * nothing we can do */
    if (NULL != info) {
        for (n=0; n < ninfo; n++) {
            if (0 == strncmp(info[n].key, PMIX_JOB_TERM_STATUS, PMIX_MAX_KEYLEN)) {
                jobstatus = opal_pmix_convert_status(info[n].value.data.status);
            } else if (0 == strncmp(info[n].key, PMIX_EVENT_AFFECTED_PROC, PMIX_MAX_KEYLEN)) {
                OPAL_PMIX_CONVERT_NSPACE(rc, &jobid, info[n].value.data.proc->nspace);
                if (ORTE_SUCCESS != rc) {
                    ORTE_ERROR_LOG(rc);
                }
            } else if (0 == strncmp(info[n].key, PMIX_EVENT_RETURN_OBJECT, PMIX_MAX_KEYLEN)) {
                lock = (opal_pmix_lock_t*)info[n].value.data.ptr;
        #ifdef PMIX_EVENT_TEXT_MESSAGE
            } else if (0 == strncmp(info[n].key, PMIX_EVENT_TEXT_MESSAGE, PMIX_MAX_KEYLEN)) {
                msg = info[n].value.data.string;
        #endif
            }
        }
        if (orte_cmd_options.verbose && (myjobid != ORTE_JOBID_INVALID && jobid == myjobid)) {
            opal_output(0, "JOB %s COMPLETED WITH STATUS %d",
                        ORTE_JOBID_PRINT(jobid), jobstatus);
        }
    }
    /* save the status */
    lock->status = jobstatus;
    if (NULL != msg) {
        lock->msg = strdup(msg);
    }
    /* release the lock */
    OPAL_PMIX_WAKEUP_THREAD(lock);

    /* we _always_ have to execute the evhandler callback or
     * else the event progress engine will hang */
    if (NULL != cbfunc) {
        cbfunc(PMIX_SUCCESS, NULL, 0, NULL, NULL, cbdata);
    }
}

/**
 * Static functions used to configure the interactions between the OPAL and
 * the runtime.
 */

static char*
_process_name_print_for_opal(const opal_process_name_t procname)
{
    orte_process_name_t* rte_name = (orte_process_name_t*)&procname;
    return ORTE_NAME_PRINT(rte_name);
}

static char*
_jobid_print_for_opal(const opal_jobid_t jobid)
{
    return ORTE_JOBID_PRINT(jobid);
}

static char*
_vpid_print_for_opal(const opal_vpid_t vpid)
{
    return ORTE_VPID_PRINT(vpid);
}

static int
_process_name_compare(const opal_process_name_t p1, const opal_process_name_t p2)
{
    return orte_util_compare_name_fields(ORTE_NS_CMP_ALL, &p1, &p2);
}

static int _convert_string_to_process_name(opal_process_name_t *name,
                                           const char* name_string)
{
    return orte_util_convert_string_to_process_name(name, name_string);
}

static int _convert_process_name_to_string(char** name_string,
                                          const opal_process_name_t *name)
{
    return orte_util_convert_process_name_to_string(name_string, name);
}

static int
_convert_string_to_jobid(opal_jobid_t *jobid, const char *jobid_string)
{
    return orte_util_convert_string_to_jobid(jobid, jobid_string);
}

int pterminate(int argc, char *argv[])
{
    int rc=1, i;
    char *param;
    opal_pmix_lock_t lock, rellock;
    opal_list_t apps;
    opal_list_t tinfo;
    pmix_info_t info, *iptr;
    pmix_status_t ret;
    bool flag;
    opal_ds_info_t *ds;
    size_t n, ninfo;

    /* init the globals */
    memset(&orte_cmd_options, 0, sizeof(orte_cmd_options));
    memset(&myoptions, 0, sizeof(myoptions));
    OBJ_CONSTRUCT(&job_info, opal_list_t);
    OBJ_CONSTRUCT(&apps, opal_list_t);

    /* Convince OPAL to use our naming scheme */
    opal_process_name_print = _process_name_print_for_opal;
    opal_vpid_print = _vpid_print_for_opal;
    opal_jobid_print = _jobid_print_for_opal;
    opal_compare_proc = _process_name_compare;
    opal_convert_string_to_process_name = _convert_string_to_process_name;
    opal_convert_process_name_to_string = _convert_process_name_to_string;
    opal_snprintf_jobid = orte_util_snprintf_jobid;
    opal_convert_string_to_jobid = _convert_string_to_jobid;

    /* search the argv for MCA params */
    for (i=0; NULL != argv[i]; i++) {
        if (':' == argv[i][0] ||
            NULL == argv[i+1] || NULL == argv[i+2]) {
            break;
        }
        if (0 == strncmp(argv[i], "-"OPAL_MCA_CMD_LINE_ID, strlen("-"OPAL_MCA_CMD_LINE_ID)) ||
            0 == strncmp(argv[i], "--"OPAL_MCA_CMD_LINE_ID, strlen("--"OPAL_MCA_CMD_LINE_ID)) ||
            0 == strncmp(argv[i], "-g"OPAL_MCA_CMD_LINE_ID, strlen("-g"OPAL_MCA_CMD_LINE_ID)) ||
            0 == strncmp(argv[i], "--g"OPAL_MCA_CMD_LINE_ID, strlen("--g"OPAL_MCA_CMD_LINE_ID))) {
            (void) mca_base_var_env_name (argv[i+1], &param);
            opal_setenv(param, argv[i+2], true, &environ);
            free(param);
        } else if (0 == strcmp(argv[i], "-am") ||
                   0 == strcmp(argv[i], "--am")) {
            (void)mca_base_var_env_name("mca_base_param_file_prefix", &param);
            opal_setenv(param, argv[i+1], true, &environ);
            free(param);
        } else if (0 == strcmp(argv[i], "-tune") ||
                   0 == strcmp(argv[i], "--tune")) {
            (void)mca_base_var_env_name("mca_base_envar_file_prefix", &param);
            opal_setenv(param, argv[i+1], true, &environ);
            free(param);
        }
    }

    /* init OPAL */
    if (OPAL_SUCCESS != (rc = opal_init(&argc, &argv))) {
        return rc;
    }

    /* set our proc type for schizo selection */
    orte_process_info.proc_type = ORTE_PROC_TOOL;

    /* open the SCHIZO framework so we can setup the command line */
    if (ORTE_SUCCESS != (rc = mca_base_framework_open(&orte_schizo_base_framework, 0))) {
        ORTE_ERROR_LOG(rc);
        return rc;
    }
    if (ORTE_SUCCESS != (rc = orte_schizo_base_select())) {
        ORTE_ERROR_LOG(rc);
        return rc;
    }

    /* setup our cmd line */
    orte_cmd_line = OBJ_NEW(opal_cmd_line_t);
    if (OPAL_SUCCESS != (rc = opal_cmd_line_add(orte_cmd_line, cmd_line_init))) {
        return rc;
    }

    /* setup the rest of the cmd line only once */
    if (OPAL_SUCCESS != (rc = orte_schizo.define_cli(orte_cmd_line))) {
        return rc;
    }

    /* now that options have been defined, finish setup */
    mca_base_cmd_line_setup(orte_cmd_line);

    /* parse the result to get values */
    if (OPAL_SUCCESS != (rc = opal_cmd_line_parse(orte_cmd_line,
                                                  true, false, argc, argv)) ) {
        if (OPAL_ERR_SILENT != rc) {
            fprintf(stderr, "%s: command line error (%s)\n", argv[0],
                    opal_strerror(rc));
        }
        return rc;
    }

    /* see if print version is requested. Do this before
     * check for help so that --version --help works as
     * one might expect. */
     if (orte_cmd_options.version) {
        char *str;
        str = opal_info_make_version_str("all",
                                         OPAL_MAJOR_VERSION, OPAL_MINOR_VERSION,
                                         OPAL_RELEASE_VERSION,
                                         OPAL_GREEK_VERSION,
                                         OPAL_REPO_REV);
        if (NULL != str) {
            fprintf(stdout, "%s (%s) %s\n\nReport bugs to %s\n",
                    "prun", "PMIx Reference Server", str, PACKAGE_BUGREPORT);
            free(str);
        }
        exit(0);
    }

    /* process any mca params */
    rc = mca_base_cmd_line_process_args(orte_cmd_line, &environ, &environ);
    if (ORTE_SUCCESS != rc) {
        return rc;
    }

    /* Check for help request */
    if (orte_cmd_options.help) {
        char *str, *args = NULL;
        args = opal_cmd_line_get_usage_msg(orte_cmd_line);
        str = opal_show_help_string("help-pterminate.txt", "pterminate:usage", false,
                                    "pterminate", "PSVR", OPAL_VERSION,
                                    "pterminate", args,
                                    PACKAGE_BUGREPORT);
        if (NULL != str) {
            printf("%s", str);
            free(str);
        }
        free(args);

        /* If someone asks for help, that should be all we do */
        exit(0);
    }

    /* setup options */
    OBJ_CONSTRUCT(&tinfo, opal_list_t);
    if (myoptions.do_not_connect) {
        ds = OBJ_NEW(opal_ds_info_t);
        PMIX_INFO_CREATE(ds->info, 1);
        PMIX_INFO_LOAD(ds->info, PMIX_TOOL_DO_NOT_CONNECT, NULL, PMIX_BOOL);
        opal_list_append(&tinfo, &ds->super);
    } else if (myoptions.system_server_first) {
        ds = OBJ_NEW(opal_ds_info_t);
        PMIX_INFO_CREATE(ds->info, 1);
        PMIX_INFO_LOAD(ds->info, PMIX_CONNECT_SYSTEM_FIRST, NULL, PMIX_BOOL);
        opal_list_append(&tinfo, &ds->super);
    } else if (myoptions.system_server_only) {
        ds = OBJ_NEW(opal_ds_info_t);
        PMIX_INFO_CREATE(ds->info, 1);
        PMIX_INFO_LOAD(ds->info, PMIX_CONNECT_TO_SYSTEM, NULL, PMIX_BOOL);
        opal_list_append(&tinfo, &ds->super);
    }
    if (0 < myoptions.wait_to_connect) {
        ds = OBJ_NEW(opal_ds_info_t);
        PMIX_INFO_CREATE(ds->info, 1);
        PMIX_INFO_LOAD(ds->info, PMIX_CONNECT_RETRY_DELAY, &myoptions.wait_to_connect, PMIX_UINT32);
        opal_list_append(&tinfo, &ds->super);
    }
    if (0 < myoptions.num_retries) {
        ds = OBJ_NEW(opal_ds_info_t);
        PMIX_INFO_CREATE(ds->info, 1);
        PMIX_INFO_LOAD(ds->info, PMIX_CONNECT_MAX_RETRIES, &myoptions.num_retries, PMIX_UINT32);
        opal_list_append(&tinfo, &ds->super);
    }
    if (0 < myoptions.pid) {
        ds = OBJ_NEW(opal_ds_info_t);
        PMIX_INFO_CREATE(ds->info, 1);
        PMIX_INFO_LOAD(ds->info, PMIX_SERVER_PIDINFO, &myoptions.pid, PMIX_PID);
        opal_list_append(&tinfo, &ds->super);
    }
    /* ensure we don't try to use the usock PTL component */
    ds = OBJ_NEW(opal_ds_info_t);
    PMIX_INFO_CREATE(ds->info, 1);
    PMIX_INFO_LOAD(ds->info, PMIX_USOCK_DISABLE, NULL, PMIX_BOOL);
    opal_list_append(&tinfo, &ds->super);

    /* we are also a launcher, so pass that down so PMIx knows
     * to setup rendezvous points */
    ds = OBJ_NEW(opal_ds_info_t);
    PMIX_INFO_CREATE(ds->info, 1);
    PMIX_INFO_LOAD(ds->info, PMIX_LAUNCHER, NULL, PMIX_BOOL);
    opal_list_append(&tinfo, &ds->super);
    /* we always support session-level rendezvous */
    ds = OBJ_NEW(opal_ds_info_t);
    PMIX_INFO_CREATE(ds->info, 1);
    PMIX_INFO_LOAD(ds->info, PMIX_SERVER_TOOL_SUPPORT, NULL, PMIX_BOOL);
    opal_list_append(&tinfo, &ds->super);
    /* use only one listener */
    ds = OBJ_NEW(opal_ds_info_t);
    PMIX_INFO_CREATE(ds->info, 1);
    PMIX_INFO_LOAD(ds->info, PMIX_SINGLE_LISTENER, NULL, PMIX_BOOL);
    opal_list_append(&tinfo, &ds->super);

    /* if they specified the URI, then pass it along */
    if (NULL != orte_cmd_options.hnp) {
        ds = OBJ_NEW(opal_ds_info_t);
        PMIX_INFO_CREATE(ds->info, 1);
        PMIX_INFO_LOAD(ds->info, PMIX_SERVER_URI, orte_cmd_options.hnp, PMIX_STRING);
        opal_list_append(&tinfo, &ds->super);
    }

    /* if we were launched by a debugger, then we need to have
     * notification of our termination sent */
    if (NULL != getenv("PMIX_LAUNCHER_PAUSE_FOR_TOOL")) {
        ds = OBJ_NEW(opal_ds_info_t);
        PMIX_INFO_CREATE(ds->info, 1);
        flag = false;
        PMIX_INFO_LOAD(ds->info, PMIX_EVENT_SILENT_TERMINATION, &flag, PMIX_BOOL);
        opal_list_append(&tinfo, &ds->super);
    }

#ifdef PMIX_LAUNCHER_RENDEZVOUS_FILE
    /* check for request to drop a rendezvous file */
    if (NULL != (param = getenv("PMIX_LAUNCHER_RENDEZVOUS_FILE"))) {
        ds = OBJ_NEW(opal_ds_info_t);
        PMIX_INFO_CREATE(ds->info, 1);
        PMIX_INFO_LOAD(ds->info, PMIX_LAUNCHER_RENDEZVOUS_FILE, param, PMIX_STRING);
        opal_list_append(&tinfo, &ds->super);
    }
#endif

    /* convert to array of info */
    ninfo = opal_list_get_size(&tinfo);
    PMIX_INFO_CREATE(iptr, ninfo);
    n = 0;
    OPAL_LIST_FOREACH(ds, &tinfo, opal_ds_info_t) {
        PMIX_INFO_XFER(&iptr[n], ds->info);
        ++n;
    }
    OPAL_LIST_DESTRUCT(&tinfo);

    /* now initialize PMIx - we have to indicate we are a launcher so that we
     * will provide rendezvous points for tools to connect to us */
    if (PMIX_SUCCESS != (ret = PMIx_tool_init(&myproc, iptr, ninfo))) {
        PMIX_ERROR_LOG(ret);
        opal_progress_thread_finalize(NULL);
        return ret;
    }
    PMIX_INFO_FREE(iptr, ninfo);

    /* setup a lock to track the connection */
    OPAL_PMIX_CONSTRUCT_LOCK(&rellock);
    /* register to trap connection loss */
    pmix_status_t code[2] = {PMIX_ERR_UNREACH, PMIX_ERR_LOST_CONNECTION_TO_SERVER};
    OPAL_PMIX_CONSTRUCT_LOCK(&lock);
    PMIX_INFO_LOAD(&info, PMIX_EVENT_RETURN_OBJECT, &rellock, PMIX_POINTER);
    PMIx_Register_event_handler(code, 2, &info, 1,
                                evhandler, regcbfunc, &lock);
    OPAL_PMIX_WAIT_THREAD(&lock);
    OPAL_PMIX_DESTRUCT_LOCK(&lock);
    flag = true;
    PMIX_INFO_LOAD(&info, PMIX_JOB_CTRL_TERMINATE, &flag, PMIX_BOOL);
    fprintf(stderr, "TERMINATING DVM...");
    OPAL_PMIX_CONSTRUCT_LOCK(&lock);
    PMIx_Job_control_nb(NULL, 0, &info, 1, infocb, (void*)&lock);
#if PMIX_VERSION_MAJOR == 3 && PMIX_VERSION_MINOR == 0 && PMIX_VERSION_RELEASE < 3
    /* There is a bug in PMIx 3.0.0 up to 3.0.2 that causes the callback never
     * being called when the server successes. The callback might be eventually
     * called though then the connection to the server closes with
     * status PMIX_ERR_COMM_FAILURE */
    poll(NULL, 0, 1000);
    infocb(PMIX_SUCCESS, NULL, 0, (void *)&lock, NULL, NULL);
#endif
    OPAL_PMIX_WAIT_THREAD(&lock);
    OPAL_PMIX_DESTRUCT_LOCK(&lock);
    /* wait for connection to depart */
    OPAL_PMIX_WAIT_THREAD(&rellock);
    OPAL_PMIX_DESTRUCT_LOCK(&rellock);
    /* wait for the connection to go away */
    fprintf(stderr, "DONE\n");
#if PMIX_VERSION_MAJOR != 3 || PMIX_VERSION_MINOR != 0 || PMIX_VERSION_RELEASE > 2
    /* cleanup and leave */
    PMIx_tool_finalize();
    opal_progress_thread_finalize(NULL);
    opal_finalize();
#endif
    return rc;
}
