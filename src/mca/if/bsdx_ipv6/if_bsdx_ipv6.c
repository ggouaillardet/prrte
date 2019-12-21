/*
 * Copyright (c) 2010      Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2010      Oracle and/or its affiliates.  All rights reserved.
 * Copyright (c) 2018      Amazon.com, Inc. or its affiliates.  All Rights
 *                         reserved.
 * Copyright (c) 2019      Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prrte_config.h"
#include "constants.h"
#include "src/util/output.h"
#include "src/util/string_copy.h"

#include <string.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <errno.h>
#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif
#ifdef HAVE_SYS_SOCKET_H
#include <sys/socket.h>
#endif
#ifdef HAVE_SYS_SOCKIO_H
#include <sys/sockio.h>
#endif
#ifdef HAVE_SYS_IOCTL_H
#include <sys/ioctl.h>
#endif
#ifdef HAVE_NETINET_IN_H
#include <netinet/in.h>
#endif
#ifdef HAVE_ARPA_INET_H
#include <arpa/inet.h>
#endif
#ifdef HAVE_NET_IF_H
#include <net/if.h>
#endif
#ifdef HAVE_NETDB_H
#include <netdb.h>
#endif
#ifdef HAVE_IFADDRS_H
#include <ifaddrs.h>
#endif

#include "src/mca/if/if.h"
#include "src/mca/if/base/base.h"

static int if_bsdx_ipv6_open(void);

/* Discovers IPv6 interfaces for:
 *
 * NetBSD
 * OpenBSD
 * FreeBSD
 * 386BSD
 * bsdi
 * Apple
 */
prrte_if_base_component_t prrte_if_bsdx_ipv6_component = {
    /* First, the mca_component_t struct containing meta information
       about the component itself */
    {
        PRRTE_IF_BASE_VERSION_2_0_0,

        /* Component name and version */
        "bsdx_ipv6",
        PRRTE_MAJOR_VERSION,
        PRRTE_MINOR_VERSION,
        PRRTE_RELEASE_VERSION,

        /* Component open and close functions */
        if_bsdx_ipv6_open,
        NULL
    },
    {
        /* This component is checkpointable */
        PRRTE_MCA_BASE_METADATA_PARAM_CHECKPOINT
    },
};

/* configure using getifaddrs(3) */
static int if_bsdx_ipv6_open(void)
{
#if PRRTE_ENABLE_IPV6
    struct ifaddrs **ifadd_list;
    struct ifaddrs *cur_ifaddrs;
    struct sockaddr_in6* sin_addr;

    prrte_output_verbose(1, prrte_if_base_framework.framework_output,
                        "searching for IPv6 interfaces");

    /*
     * the manpage claims that getifaddrs() allocates the memory,
     * and freeifaddrs() is later used to release the allocated memory.
     * however, without this malloc the call to getifaddrs() segfaults
     */
    ifadd_list = (struct ifaddrs **) malloc(sizeof(struct ifaddrs*));

    /* create the linked list of ifaddrs structs */
    if (getifaddrs(ifadd_list) < 0) {
        prrte_output(0, "prrte_ifinit: getifaddrs() failed with error=%d\n",
                    errno);
        free(ifadd_list);
        return PRRTE_ERROR;
    }

    for (cur_ifaddrs = *ifadd_list; NULL != cur_ifaddrs;
         cur_ifaddrs = cur_ifaddrs->ifa_next) {
        prrte_if_t *intf;
        struct in6_addr a6;

        /* skip non-ipv6 interface addresses */
        if (AF_INET6 != cur_ifaddrs->ifa_addr->sa_family) {
            prrte_output_verbose(1, prrte_if_base_framework.framework_output,
                                "skipping non-ipv6 interface %s[%d].\n",
                                cur_ifaddrs->ifa_name, (int)cur_ifaddrs->ifa_addr->sa_family);
            continue;
        }

        /* skip interface if it is down (IFF_UP not set) */
        if (0 == (cur_ifaddrs->ifa_flags & IFF_UP)) {
            prrte_output_verbose(1, prrte_if_base_framework.framework_output,
                                "skipping non-up interface %s.\n", cur_ifaddrs->ifa_name);
            continue;
        }

        /* skip interface if it is a loopback device (IFF_LOOPBACK set) */
        if (!prrte_if_retain_loopback && 0 != (cur_ifaddrs->ifa_flags & IFF_LOOPBACK)) {
            prrte_output_verbose(1, prrte_if_base_framework.framework_output,
                                "skipping loopback interface %s.\n", cur_ifaddrs->ifa_name);
            continue;
        }

        /* or if it is a point-to-point interface */
        /* TODO: do we really skip p2p? */
        if (0!= (cur_ifaddrs->ifa_flags & IFF_POINTOPOINT)) {
            prrte_output_verbose(1, prrte_if_base_framework.framework_output,
                                "skipping p2p interface %s.\n", cur_ifaddrs->ifa_name);
            continue;
        }

        sin_addr = (struct sockaddr_in6 *) cur_ifaddrs->ifa_addr;

        /*
         * skip IPv6 address starting with fe80:, as this is supposed to be
         * link-local scope. sockaddr_in6->sin6_scope_id doesn't always work
         * TODO: test whether scope id is set to a sensible value on
         * linux and/or bsd (including osx)
         *
         * MacOSX: fe80::... has a scope of 0, but ifconfig -a shows
         * a scope of 4 on that particular machine,
         * so the scope returned by getifaddrs() isn't working properly
         */

        if ((IN6_IS_ADDR_LINKLOCAL (&sin_addr->sin6_addr))) {
            prrte_output_verbose(1, prrte_if_base_framework.framework_output,
                                "skipping link-local ipv6 address on interface "
                                "%s with scope %d.\n",
                                cur_ifaddrs->ifa_name, sin_addr->sin6_scope_id);
            continue;
        }

        if (0 < prrte_output_get_verbosity(prrte_if_base_framework.framework_output)) {
            char *addr_name = (char *) malloc(48*sizeof(char));
            inet_ntop(AF_INET6, &sin_addr->sin6_addr, addr_name, 48*sizeof(char));
            prrte_output(0, "ipv6 capable interface %s discovered, address %s.\n",
                        cur_ifaddrs->ifa_name, addr_name);
            free(addr_name);
        }

        /* fill values into the prrte_if_t */
        memcpy(&a6, &(sin_addr->sin6_addr), sizeof(struct in6_addr));

        intf = PRRTE_NEW(prrte_if_t);
        if (NULL == intf) {
            prrte_output(0, "prrte_ifinit: unable to allocate %lu bytes\n",
                        sizeof(prrte_if_t));
            free(ifadd_list);
            return PRRTE_ERR_OUT_OF_RESOURCE;
        }
        intf->af_family = AF_INET6;
        prrte_string_copy(intf->if_name, cur_ifaddrs->ifa_name, PRRTE_IF_NAMESIZE);
        intf->if_index = prrte_list_get_size(&prrte_if_list) + 1;
        ((struct sockaddr_in6*) &intf->if_addr)->sin6_addr = a6;
        ((struct sockaddr_in6*) &intf->if_addr)->sin6_family = AF_INET6;

        /* since every scope != 0 is ignored, we just set the scope to 0 */
        ((struct sockaddr_in6*) &intf->if_addr)->sin6_scope_id = 0;

        /*
         * hardcoded netmask, adrian says that's ok
         */
        intf->if_mask = 64;
        intf->if_flags = cur_ifaddrs->ifa_flags;

        /*
         * FIXME: figure out how to gain access to the kernel index
         * (or create our own), getifaddrs() does not contain such
         * data
         */
        intf->if_kernel_index =
            (uint16_t) if_nametoindex(cur_ifaddrs->ifa_name);
        prrte_list_append(&prrte_if_list, &(intf->super));
    }   /*  of for loop over ifaddrs list */

    free(ifadd_list);
#endif  /* PRRTE_ENABLE_IPV6 */

    return PRRTE_SUCCESS;
}


