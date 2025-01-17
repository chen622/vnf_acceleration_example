/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2017 Mellanox Technologies, Ltd
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <sys/types.h>
#include <sys/queue.h>
#include <netinet/in.h>
#include <setjmp.h>
#include <stdarg.h>
#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdbool.h>

#include <rte_eal.h>
#include <rte_common.h>
#include <rte_malloc.h>
#include <rte_ether.h>
#include <rte_ethdev.h>
#include <rte_mempool.h>
#include <rte_mbuf.h>
#include <rte_net.h>
#include <rte_flow.h>
#include <rte_cycles.h>

#include "vnf_examples.h"

static volatile bool force_quit;

static uint16_t port_id;
static uint32_t nr_std_queues = 8;
static uint16_t queues[] = {1, 3, 2, 4, 5, 7, 0, 6};
struct rte_mempool *mbuf_pool;
struct rte_flow *flow;
static uint16_t nr_hairpin_queues = 1;

#define SRC_IP ((0<<24) + (0<<16) + (0<<8) + 0) /* src ip = 0.0.0.0 */
#define DEST_IP ((192<<24) + (168<<16) + (1<<8) + 1) /* dest ip = 192.168.1.1 */
#define FULL_MASK 0xffffffff /* full mask */
#define EMPTY_MASK 0x0 /* empty mask */

static inline void
print_ether_addr(const char *what, struct rte_ether_addr *eth_addr) {
    char buf[RTE_ETHER_ADDR_FMT_SIZE];
    rte_ether_format_addr(buf, RTE_ETHER_ADDR_FMT_SIZE, eth_addr);
    printf("%s%s", what, buf);
}

static inline void print_ip_addr(const char *what, const rte_be32_t *ip_addr) {
    uint32_t ip = rte_be_to_cpu_32(*ip_addr);
    printf("%s%d.%d.%d.%d", what,
           (ip >> 24) & 0x000000ff,
           (ip >> 16) & 0x000000ff,
           (ip >> 8) & 0x000000ff,
           (ip) & 0x000000ff);
}

static inline void
dump_pkt_info(struct rte_mbuf *m, uint16_t qi) {
    struct rte_ether_hdr *eth_hdr;
    struct rte_ipv4_hdr *ip_hdr;

    eth_hdr = rte_pktmbuf_mtod(m, struct rte_ether_hdr *);
    print_ether_addr("src_mac=", &eth_hdr->s_addr);
    print_ether_addr(" - dst_mac=", &eth_hdr->d_addr);
    rte_pktmbuf_adj(m, (uint16_t) sizeof(struct rte_ether_hdr));
    ip_hdr = rte_pktmbuf_mtod(m, struct rte_ipv4_hdr *);

    print_ip_addr(" - src_ip=", &ip_hdr->src_addr);
    print_ip_addr(" - dst_ip=", &ip_hdr->dst_addr);
    printf(" - queue=0x%x", (unsigned int) qi);
    uint64_t ol_flags = m->ol_flags;
    if (ol_flags & PKT_RX_RSS_HASH) {
        printf(" - RSS hash=0x%x", (unsigned int) m->hash.rss);
        printf(" - RSS queue=0x%x", (unsigned int) qi);
    }
    if (ol_flags & PKT_RX_FDIR) {
        printf(" - FDIR matched ");
        if (ol_flags & PKT_RX_FDIR_ID)
            printf("ID=0x%x", m->hash.fdir.hi);
        else if (ol_flags & PKT_RX_FDIR_FLX)
            printf("flex bytes=0x%08x %08x",
                   m->hash.fdir.hi, m->hash.fdir.lo);
        else
            printf("hash=0x%x ID=0x%x ",
                   m->hash.fdir.hash, m->hash.fdir.id);
    }
    printf("\n");
}

static int
main_loop(void *lcore_port_id) {
    struct rte_mbuf *mbufs[32];
    uint16_t nb_rx;
    uint16_t nb_tx;
    uint16_t i;
    uint16_t j;

    while (!force_quit) {
        for (i = 0; i < nr_std_queues; i++) {
            nb_rx = rte_eth_rx_burst(*(uint16_t *) lcore_port_id,
                                     i, mbufs, 32);
            if (nb_rx) {
                for (j = 0; j < nb_rx; j++) {
                    struct rte_mbuf *m = mbufs[j];
                    if (query_counters(*(uint16_t *) lcore_port_id)) {
                        printf("Failed to query counters\n");
                        rte_exit(EXIT_FAILURE, "error to sync flows");
                    }
                    dump_pkt_info(m, i);
                }
                nb_tx = rte_eth_tx_burst(*(uint16_t *) lcore_port_id, i,
                                         mbufs, nb_rx);
            }
            /* Free any unsent packets. */
            if (unlikely(nb_tx < nb_rx)) {
                uint16_t buf;
                for (buf = nb_tx; buf < nb_rx; buf++)
                    rte_pktmbuf_free(mbufs[buf]);
            }
        }
    }

    return 0;
}

#define CHECK_INTERVAL 1000  /* 100ms */
#define MAX_REPEAT_TIMES 90  /* 9s (90 * 100ms) in total */

static void
assert_link_status(void) {
    struct rte_eth_link link;
    uint8_t rep_cnt = MAX_REPEAT_TIMES;
    int link_get_err = -EINVAL;

    memset(&link, 0, sizeof(link));
    do {
        link_get_err = rte_eth_link_get(port_id, &link);
        if (link_get_err == 0 && link.link_status == ETH_LINK_UP)
            break;
        rte_delay_ms(CHECK_INTERVAL);
    } while (--rep_cnt);

    if (link_get_err < 0)
        rte_exit(EXIT_FAILURE, ":: error: link get is failing: %s\n",
                 rte_strerror(-link_get_err));
    if (link.link_status == ETH_LINK_DOWN)
        rte_exit(EXIT_FAILURE, ":: error: link is still down\n");
}

static void
init_port(uint16_t port_id) {
    int ret;
    uint16_t i;
    struct rte_eth_conf port_conf = {
            .rxmode = {
                    .split_hdr_size = 0,
            },
            .txmode = {
                    .offloads =
                    DEV_TX_OFFLOAD_VLAN_INSERT |
                    DEV_TX_OFFLOAD_IPV4_CKSUM |
                    DEV_TX_OFFLOAD_UDP_CKSUM |
                    DEV_TX_OFFLOAD_TCP_CKSUM |
                    DEV_TX_OFFLOAD_SCTP_CKSUM |
                    DEV_TX_OFFLOAD_TCP_TSO,
            },
    };
    struct rte_eth_txconf txq_conf;
    struct rte_eth_rxconf rxq_conf;
    struct rte_eth_dev_info dev_info;

    ret = rte_eth_dev_info_get(port_id, &dev_info);
    if (ret != 0)
        rte_exit(EXIT_FAILURE,
                 "Error during getting device (port %u) info: %s\n",
                 port_id, strerror(-ret));

    port_conf.txmode.offloads &= dev_info.tx_offload_capa;
    printf(":: initializing port: %d\n", port_id);
    ret = rte_eth_dev_configure(port_id,
                                nr_std_queues + nr_hairpin_queues,
                                nr_std_queues + nr_hairpin_queues, &port_conf);
    if (ret < 0) {
        rte_exit(EXIT_FAILURE,
                 ":: cannot configure device: err=%d, port=%u\n",
                 ret, port_id);
    }

    rxq_conf = dev_info.default_rxconf;
    rxq_conf.offloads = port_conf.rxmode.offloads;
    for (i = 0; i < nr_std_queues; i++) {
        ret = rte_eth_rx_queue_setup(port_id, i, 512,
                                     rte_eth_dev_socket_id(port_id),
                                     &rxq_conf,
                                     mbuf_pool);
        if (ret < 0) {
            rte_exit(EXIT_FAILURE,
                     ":: Rx queue setup failed: err=%d, port=%u\n",
                     ret, port_id);
        }
    }

    txq_conf = dev_info.default_txconf;
    txq_conf.offloads = port_conf.txmode.offloads;

    for (i = 0; i < nr_std_queues; i++) {
        ret = rte_eth_tx_queue_setup(port_id, i, 512,
                                     rte_eth_dev_socket_id(port_id),
                                     &txq_conf);
        if (ret < 0) {
            rte_exit(EXIT_FAILURE,
                     ":: Tx queue setup failed: err=%d, port=%u\n",
                     ret, port_id);
        }
    }

    ret = rte_eth_promiscuous_enable(port_id);
    if (ret != 0)
        rte_exit(EXIT_FAILURE,
                 ":: promiscuous mode enable failed: err=%s, port=%u\n",
                 rte_strerror(-ret), port_id);


    printf(":: initializing port: %d done\n", port_id);
}

static void
init_ports(void) {
    uint16_t port_id;

    RTE_ETH_FOREACH_DEV(port_id) {
        init_port(port_id);
    }
}

static void
start_ports(void) {
    uint16_t port_id;
    int ret;

    RTE_ETH_FOREACH_DEV(port_id) {
        ret = rte_eth_dev_start(port_id);
        if (ret < 0) {
            rte_exit(EXIT_FAILURE,
                     "rte_eth_dev_start:err=%d, port=%u\n",
                     ret, port_id);
        }
        assert_link_status();
    }

}

static void
signal_handler(int signum) {
    if (signum == SIGINT || signum == SIGTERM) {
        printf("\n\nSignal %d received, preparing to exit...\n",
               signum);
        force_quit = true;
    }
}

int
main(int argc, char **argv) {
    int ret;
    uint16_t nr_ports;


    ret = rte_eal_init(argc, argv);
    if (ret < 0)
        rte_exit(EXIT_FAILURE, ":: invalid EAL arguments\n");

    force_quit = false;
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    nr_ports = rte_eth_dev_count_avail();
    if (nr_ports == 0)
        rte_exit(EXIT_FAILURE, ":: no Ethernet ports found\n");
    port_id = 0;
    if (nr_ports != 1 && nr_ports != 2) {
        printf(":: warn: %d ports detected, but we use two ports at max\n",
               nr_ports);
    }
    mbuf_pool = rte_pktmbuf_pool_create("mbuf_pool", 40960, 128, 0,
                                        RTE_MBUF_DEFAULT_BUF_SIZE,
                                        rte_socket_id());
    if (mbuf_pool == NULL)
        rte_exit(EXIT_FAILURE, "Cannot init mbuf pool\n");

    init_ports();
    printf(":: %u ports active, setup %u ports hairpin...",
           nr_ports, nr_ports);
    if (nr_ports == 2)
        hairpin_two_ports_setup(nr_hairpin_queues);
    else
        hairpin_one_port_setup(port_id, nr_hairpin_queues);
    printf("done\n");
    start_ports();
    printf(":: %u ports hairpin bind...", nr_ports);
    if (nr_ports == 2) {
        ret = hairpin_two_ports_bind();
        if (ret)
            rte_exit(EXIT_FAILURE, "Cannot bind two hairpin ports");
    }
    printf("done\n");
    port_id = rte_eth_find_next(0);
    printf(":: warning: only use first port: %u\n", port_id);
    /* create flow for send packet with */
    flow = create_gtp_u_decap_rss_flow(port_id, nr_std_queues,
                                       queues);
    flow = create_gtp_u_inner_ip_rss_flow(port_id, nr_std_queues,
                                          queues);
    flow = create_gtp_u_encap_flow(port_id);
    flow = create_gtp_u_psc_encap_flow(port_id);
    if (!flow) {
        printf("Flow can't be created \n");
        rte_exit(EXIT_FAILURE, "error in creating flow");
    }
    printf(":: create hairpin flows...");
    if (nr_ports == 2)
        flow = hairpin_two_ports_flows_create();
    else
        flow = hairpin_one_port_flows_create();

    if (!flow) {
        printf("Hairpin flows can't be created\n");
        rte_exit(EXIT_FAILURE, "error in creating flow");
    }
    printf("done\n");
    printf(":: create flow using tag...");
    flow = create_flow_with_tag(port_id);
    if (!flow) {
        printf("Flow with TAG cannot be created\n");
        rte_exit(EXIT_FAILURE, "error in creating flow");
    }
    printf("done\n");
    printf(":: create flow with sampling action...");
    flow = create_flow_with_sampling(port_id);
    if (!flow) {
        printf("Flow with sampling cannot be created\n");
        rte_exit(EXIT_FAILURE, "error in creating flow");
    }
    printf("done\n");
    printf(":: create flow with sampling action...");
    flow = create_nic_flow_with_mirror(port_id, 2, 1);
    if (!flow) {
        printf("Flow with mirror on NIC RX domain cannot be created\n");
        rte_exit(EXIT_FAILURE, "error in creating flow");
    }
    printf("done\n");
    printf(":: create flow with symmetric RSS action...");
    if (create_symmetric_rss_flow(port_id, nr_std_queues, queues)) {
        printf("Flow with symmetric RSS cannot be created\n");
        rte_exit(EXIT_FAILURE, "error in creating flow");
    }
    printf("done\n");
//	printf(":: create flow with meter...");
//	if (create_flow_with_meter(port_id)) {
//		printf("Flow with meter cannot be created\n");
//		rte_exit(EXIT_FAILURE, "error in creating flow");
//	}
//	printf("done\n");
//    printf(":: create flow match on GTP QFI...");
//    if (create_gtp_u_qfi_flow(port_id)) {
//        printf("Flow match on GTP QFI cannot be created\n");
//        rte_exit(EXIT_FAILURE, "error in creating flow");
//    }
    printf("done\n");
    printf(":: create flow with age action...");
    if (create_flow_with_age(port_id)) {
        printf("Flow with age cannot be created\n");
        rte_exit(EXIT_FAILURE, "error in creating flow");
    }
    printf("done\n");
    printf(":: create GRE RSS flow ..");
    flow = create_gre_decap_rss_flow(port_id, nr_std_queues, queues);
    if (!flow) {
        printf("GRE RSS decap flows cannot be created\n");
        rte_exit(EXIT_FAILURE, "error in creating flow");
    }
    printf("done\n");
    printf(":: create GRE encap flow ..");
    flow = create_gre_encap_flow(port_id);
    if (!flow) {
        printf("GRE encap flow cannot be created\n");
        rte_exit(EXIT_FAILURE, "error in create flow");
    }
    printf("done\n");
    if (nr_ports == 2) {
        printf(":: create hairpin flow with meta ..");
        if (create_hairpin_meta_flow()) {
            printf("Hairpin flow with meta data cannot be created\n");
            rte_exit(EXIT_FAILURE, "error in create flow");
        }
        printf("done\n");
    }
//    printf(":: create GTP-U shared inner RSS flow ..");
//    flow = create_gtp_u_inner_ip_shared_rss_flow(port_id, nr_std_queues,
//                                                 queues);
//    if (!flow) {
//        printf("GTP-U shared inner RSS flow cannot be created\n");
//        rte_exit(EXIT_FAILURE, "error in creating flow");
//    }
//    printf("done\n");
    printf(":: create flows with counter ..");
    if (create_flow_with_counter(port_id)) {
        printf("Flows with counter cannot be created\n");
        rte_exit(EXIT_FAILURE, "error in creating flow");
    }
    printf("done\n");
//  ret = sync_all_flows(port_id);
//  if (ret) {
//    printf("Failed to sync flows, flows may not take effect!\n");
//    rte_exit(EXIT_FAILURE, "error to sync flows");
//  }

  printf(":: query counters ...\n");
  if (query_counters(port_id)) {
    printf("Failed to query counters\n");
    rte_exit(EXIT_FAILURE, "error to sync flows");
  }
  printf(":: create modify GTP-U TEID flows ..");
  ret = create_modify_gtp_teid_flows(port_id);
  if (ret) {
    printf("Failed to create modify GTP-U TEID flows\n");
    rte_exit(EXIT_FAILURE, "error in create flow");
  }
  printf("done\n");

//  unsigned lcore_id;
//  /* Launches the function on each lcore. 8< */
//  RTE_LCORE_FOREACH_WORKER(lcore_id) {
//    printf("lcore: %d", lcore_id);
//    /* Simpler equivalent. 8< */
//    rte_eal_remote_launch(main_loop, &port_id, lcore_id);
//    /* >8 End of simpler equivalent. */
//  }

    /* call it on main lcore too */
    main_loop(&port_id);
    /* >8 End of launching the function on each lcore. */

    rte_eal_mp_wait_lcore();

    /* closing and releasing resources */
    RTE_ETH_FOREACH_DEV(port_id) {
        struct rte_flow_error error;
        rte_flow_flush(port_id, &error);
    }
    if (2 == rte_eth_dev_count_avail())
        hairpin_two_ports_unbind();

    RTE_ETH_FOREACH_DEV(port_id) {
        rte_eth_dev_stop(port_id);
        rte_eth_dev_close(port_id);
    }

    /* clean up the EAL */
    rte_eal_cleanup();

    return 0;
}
