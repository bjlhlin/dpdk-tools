//1c  1p   110(42+64+4)

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>
#include <sys/types.h>
#include <string.h>
#include <sys/queue.h>
#include <stdarg.h>
#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdbool.h>
#include <time.h>
#include <sys/time.h>

#include <rte_common.h>
#include <rte_byteorder.h>
#include <rte_log.h>

#include <rte_memory.h>
#include <rte_memcpy.h>
#include <rte_memzone.h>
#include <rte_malloc.h>
#include <rte_ring.h>
#include <rte_mempool.h>
#include <rte_mbuf.h>

#include <rte_eal.h>
#include <rte_per_lcore.h>
#include <rte_launch.h>
#include <rte_atomic.h>
#include <rte_spinlock.h>
#include <rte_cycles.h>
#include <rte_prefetch.h>
#include <rte_lcore.h>
#include <rte_per_lcore.h>
#include <rte_branch_prediction.h>
#include <rte_interrupts.h>
#include <rte_pci.h>
#include <rte_random.h>
#include <rte_debug.h>
#include <rte_ether.h>
#include <rte_ethdev.h>

#include <rte_ip.h>
#include <rte_tcp.h>
#include <rte_udp.h>
#include <rte_string_fns.h>

#define MAX_PKT_BURST 32
#define NB_MBUF 8192
/* �ڴ�ػ����С */
#define MEMPOOL_CACHE_SIZE 256

#define IP_SRC_ADDR ((10U << 24) | (0 << 16) | (0 << 8) | 11)
#define IP_DST_ADDR ((10U << 24) | (0 << 16) | (0 << 8) | 21)
#define UDP_SRC_PORT 1024
#define UDP_DST_PORT 1024

#define IP_DEFTTL  64   /* from RFC 1340. */
#define IP_VERSION 0x40
#define IP_HDRLEN  0x05 /* default IP header length == five 32-bits words. */
#define IP_VHL_DEF (IP_VERSION | IP_HDRLEN)
#define SEND_TOTAL 1

static volatile bool force_quit;//����ǿ���˳���ʶ��
struct rte_mempool *pktmbuf_pool;
struct rte_mbuf *mbuf_list[MAX_PKT_BURST];//��Ӧ��rte_mbuf�ṹָ�����顣32
 
struct ether_addr des_eth_addrs;//Ŀ��mac
struct ether_addr src_eth_addrs;//Դmac
struct ether_addr eth_addrs;

static struct ipv4_hdr  pkt_ip_hdr;  /**< IP header */
static struct udp_hdr pkt_udp_hdr;   /**< UDP header*/

unsigned socket_id;
unsigned port_id;
unsigned lcore_id;
unsigned rx_queue_id;
unsigned tx_queue_id;

uint32_t send_total0 = 0;

rte_spinlock_t spinlock_conf = RTE_SPINLOCK_INITIALIZER; //������������֤��һ�����ھ������ʣ�

	
char buf[64] = {'a','a','a','a','a','a','a','a','a','a',
			    'a','a','a','a','a','a','a','a','a','a',
				'a','a','a','a','a','a','a','a','a','a',
				'a','a','a','a','a','a','a','a','a','a',
				'a','a','a','a','a','a','a','a','a','a',
				'a','a','a','a','a','a','a','a','a','a',
				'a','a','a','a'
				};

/*�ṹ�嶨��-start*/	
static struct rte_eth_conf port_conf = { //�˿�������Ϣ
	.rxmode = {
		.max_rx_pkt_len = ETHER_MAX_LEN, /**< Default maximum frame length. */
		.split_hdr_size = 0,
		.header_split   = 1, /**< Header Split disabled. */
		.hw_ip_checksum = 0, /**< IP checksum offload disabled. */
		.hw_vlan_filter = 1, /**< VLAN filtering enabled. */
		.hw_vlan_strip  = 1, /**< VLAN strip enabled. */
		.hw_vlan_extend = 0, /**< Extended VLAN disabled. */
		.jumbo_frame    = 0, /**< Jumbo Frame Support disabled. */
		.hw_strip_crc   = 1, /**< CRC stripping by hardware enabled. */
		//.mq_mode = ETH_MQ_RX_DCB_RSS,
	},
	.rx_adv_conf = {
		.rss_conf = {
			.rss_key = NULL,
			.rss_hf = ETH_RSS_IP,
		},
	},
	.txmode = {
		.mq_mode = ETH_MQ_TX_NONE,
	},
};
//�źŴ���
static void
signal_handler(int signum)
{
	if (signum == SIGINT || signum == SIGTERM) {
		printf("\n\nSignal %d received, preparing to exit...\n",
				signum);
		force_quit = true;
	}
}
//��ӡmac��ַ
static void
print_ethaddr(const char *name, const struct ether_addr *eth_addr)
{
	char buf[ETHER_ADDR_FMT_SIZE];
	ether_format_addr(buf, ETHER_ADDR_FMT_SIZE, eth_addr);
	printf("%s%s \n", name, buf);
}

static void
setup_pkt_udp_ip_headers(struct ipv4_hdr *ip_hdr,
			 struct udp_hdr *udp_hdr,
			 uint16_t pkt_data_len)
{
	uint16_t *ptr16;
	uint32_t ip_cksum;
	uint16_t pkt_len;

	/*
	 * Initialize UDP header.
	 */
	pkt_len = (uint16_t) (pkt_data_len + sizeof(struct udp_hdr));
	udp_hdr->src_port = rte_cpu_to_be_16(UDP_SRC_PORT);
	udp_hdr->dst_port = rte_cpu_to_be_16(UDP_DST_PORT);
	udp_hdr->dgram_len      = rte_cpu_to_be_16(pkt_len);
	udp_hdr->dgram_cksum    = 0; /* No UDP checksum. */

	/*
	 * Initialize IP header.
	 */
	pkt_len = (uint16_t) (pkt_len + sizeof(struct ipv4_hdr));
	ip_hdr->version_ihl   = IP_VHL_DEF;
	ip_hdr->type_of_service   = 0;
	ip_hdr->fragment_offset = 0;
	ip_hdr->time_to_live   = IP_DEFTTL;
	ip_hdr->next_proto_id = IPPROTO_UDP;
	ip_hdr->packet_id = 0;
	ip_hdr->total_length   = rte_cpu_to_be_16(pkt_len);
	ip_hdr->src_addr = rte_cpu_to_be_32(IP_SRC_ADDR);
	ip_hdr->dst_addr = rte_cpu_to_be_32(IP_DST_ADDR);

	/*
	 * Compute IP header checksum.
	 */
	ptr16 = (unaligned_uint16_t*) ip_hdr;
	ip_cksum = 0;
	ip_cksum += ptr16[0]; ip_cksum += ptr16[1];
	ip_cksum += ptr16[2]; ip_cksum += ptr16[3];
	ip_cksum += ptr16[4];
	ip_cksum += ptr16[6]; ip_cksum += ptr16[7];
	ip_cksum += ptr16[8]; ip_cksum += ptr16[9];

	/*
	 * Reduce 32 bit checksum to 16 bits and complement it.
	 */
	ip_cksum = ((ip_cksum & 0xFFFF0000) >> 16) +
		(ip_cksum & 0x0000FFFF);
	if (ip_cksum > 65535)
		ip_cksum -= 65535;
	ip_cksum = (~ip_cksum) & 0x0000FFFF;
	if (ip_cksum == 0)
		ip_cksum = 0xFFFF;
	ip_hdr->hdr_checksum = (uint16_t) ip_cksum;
}
//
static void
copy_buf_to_pkt_segs(void* buf, unsigned len, struct rte_mbuf *pkt,
		     unsigned offset)
{
	struct rte_mbuf *seg;
	void *seg_buf;
	unsigned copy_len;

	seg = pkt;
	while (offset >= seg->data_len) {
		offset -= seg->data_len;
		seg = seg->next;
	}
	copy_len = seg->data_len - offset;
	seg_buf = rte_pktmbuf_mtod_offset(seg, char *, offset);
	while (len > copy_len) {
		rte_memcpy(seg_buf, buf, (size_t) copy_len);
		len -= copy_len;
		buf = ((char*) buf + copy_len);
		seg = seg->next;
		seg_buf = rte_pktmbuf_mtod(seg, char *);
	}
	rte_memcpy(seg_buf, buf, (size_t) len);
}
static inline void
copy_buf_to_pkt(void* buf, unsigned len, struct rte_mbuf *pkt, unsigned offset)
{
	if (offset + len <= pkt->data_len) {
		rte_memcpy(rte_pktmbuf_mtod_offset(pkt, char *, offset),buf, (size_t) len);
		return;
	}
	copy_buf_to_pkt_segs(buf, len, pkt, offset);
}
static void
create_pkt_mbuf_array(uint16_t count)
{
	uint16_t i;
	struct rte_mbuf *pkt;
	struct ether_hdr eth_hdr;
	
	for(i = 0; i < MAX_PKT_BURST; i++)
	{
		//printf("alloc mbuf\n");	
		pkt =  rte_mbuf_raw_alloc(pktmbuf_pool);
			
		//pkt =  rte_pktmbuf_alloc(pktmbuf_pool);
		if (pkt == NULL) {
			printf("error: no enough pool!");
		}
		//printf("mbuf headroom reset\n");	
		rte_pktmbuf_reset_headroom(pkt);
		
		pkt->data_len = sizeof(struct ether_hdr) +sizeof(struct ipv4_hdr) + sizeof(struct udp_hdr) + sizeof(buf);//106
		//printf("-------- pkt->data_len  :%d\n",pkt->data_len);
		//pkt->data_len = sizeof(struct ether_hdr) +sizeof(struct ipv4_hdr) + sizeof(struct udp_hdr);
		pkt->next = NULL;
		
		/*
		 * ��ʼ����̫������ͷ��.
		 */
		//printf("init enther header \n");	
		ether_addr_copy(&des_eth_addrs,&eth_hdr.d_addr);
		ether_addr_copy(&src_eth_addrs, &eth_hdr.s_addr);
		eth_hdr.ether_type = rte_cpu_to_be_16(ETHER_TYPE_IPv4);
		
		/*
		 * ��ʼ�����ݰ�
		 */
		//printf("init data packet  \n"); 
		copy_buf_to_pkt(&eth_hdr, sizeof(eth_hdr), pkt, 0);//������̫��֡ͷ //14byte

		copy_buf_to_pkt(&pkt_ip_hdr, sizeof(pkt_ip_hdr), pkt,sizeof(struct ether_hdr));//����IP����ͷ //20byte

		copy_buf_to_pkt(&pkt_udp_hdr, sizeof(pkt_udp_hdr), pkt,sizeof(struct ether_hdr) +sizeof(struct ipv4_hdr));//����IPv4 ͷ�� //8byte 

		copy_buf_to_pkt(&buf, sizeof(buf), pkt, sizeof(struct ether_hdr) +sizeof(struct ipv4_hdr) + sizeof(struct udp_hdr));//�������
		
		//printf("data packet sizeof:%lu \n",sizeof(buf)+sizeof(struct ether_hdr) +sizeof(struct ipv4_hdr) + sizeof(struct udp_hdr)); 
		
		/*
		 * mbuf �������� 
		 */
		pkt->nb_segs = 0;
		pkt->pkt_len = pkt->data_len;
		//printf("-------- pkt->pkt_len  :%d\n",pkt->pkt_len);
		pkt->ol_flags = 0;
		pkt->vlan_tci = 0;
		pkt->vlan_tci_outer = 0;
		pkt->l2_len = sizeof(struct ether_hdr);
		pkt->l3_len = sizeof(struct ipv4_hdr);
		pkt->l4_len = sizeof(struct udp_hdr);
		
		mbuf_list[i] = pkt;
		
		//printf("-------- mbuf size  :%ld\n",sizeof(pkt));// 8 byte
		//printf("-------- headroom size  :%ld\n",rte_pktmbuf_headroom(pkt));// 128 byte
		//printf("-------- tailroom size  :%ld\n",rte_pktmbuf_tailroom(pkt));// 1942 byte
		
	}
}
/* ��һ�����ڷ������ݰ� */
static inline int
send_burst(uint8_t portid,uint8_t queueid)
{
	uint16_t i;
	uint16_t nb_send = 0;
	uint32_t retry;

	//printf("lock send data \n");
	rte_spinlock_lock(&spinlock_conf); //����������������
	nb_send = rte_eth_tx_burst(portid, queueid, mbuf_list, MAX_PKT_BURST); //��������
	printf("-------- data send finished  :%d\n",nb_send);
	rte_spinlock_unlock(&spinlock_conf); //�ͷ����������߳̿��Է���
	//printf("unlock data send finished \n");
	
	/*
	if (unlikely(nb_send < MAX_PKT_BURST)) {
		retry = 0;
		while (nb_send < MAX_PKT_BURST && retry++ < 100) {
			rte_delay_us(1);
			nb_send += rte_eth_tx_burst(portid, queueid, mbuf_list, MAX_PKT_BURST - nb_send);
		}
	}
	if (unlikely(nb_send < MAX_PKT_BURST)) {
		do {
			rte_pktmbuf_free(mbuf_list[nb_send]);
		} while (++nb_send < MAX_PKT_BURST);
	}
	*/
	
	//  /*
	for(i = 0; i < MAX_PKT_BURST; i++)
	{
		rte_pktmbuf_free(mbuf_list[i]);
	}
	//  */
	
    send_total0 += nb_send;
	return nb_send;
}

static int
app_lcore_main_loop(__attribute__((unused)) void *arg)
{
	unsigned lcoreid;
	uint32_t count;
	uint32_t num;
	uint16_t ret;
	uint16_t pkt_data_len = sizeof(buf);  
	
	struct rte_eth_stats port_stats;
	
	struct timeval tv;
	
	lcoreid = rte_lcore_id();
	count = 0;
	num = 0;
	if(lcoreid == lcore_id)
	{
		printf("-------send from core %u\n", lcore_id);
		
		rte_eth_stats_reset(port_id);
		
		if(!rte_eth_stats_get(port_id,&port_stats))
		{
			printf("rec_packets :%ld    sen_packets :%ld\n",port_stats.ipackets, port_stats.opackets);
			printf("rec_bytes :%ld      sen_bytes :%ld\n",port_stats.ibytes, port_stats.obytes);
			printf("rec_error :%ld      sen_error :%ld\n",port_stats.ierrors, port_stats.oerrors);
			printf("rec_missed :%ld     rx_nombuf :%ld\n",port_stats.imissed,port_stats.rx_nombuf);
				
		}
		gettimeofday(&tv,NULL);
		int starttime= tv.tv_sec*1000000 + tv.tv_usec;  //΢��
		
		/*
		 * ׼�����������ݰ�
		 */
		setup_pkt_udp_ip_headers(&pkt_ip_hdr, &pkt_udp_hdr, pkt_data_len);//��ʼ��IP UDP ����ͷ��
		
		while (num < SEND_TOTAL) 
		{
			
			if(force_quit)
				break;
			if((num+1)%16 == 0)
			{
				//rte_delay_us_block(1500);//�ӳ�1500΢��   ----512
				
			}
			create_pkt_mbuf_array(count);//��װ32�������͵�mbuf
			ret = send_burst(port_id, tx_queue_id);
			//rte_delay_ms(250);
			rte_eth_tx_done_cleanup(port_id,tx_queue_id, 0);
			count += ret;
			num++;
		}
		gettimeofday(&tv,NULL);
		int endtime= tv.tv_sec*1000000 + tv.tv_usec;  //΢��
		rte_delay_ms(1000);//�ӳ�10��
		if(!rte_eth_stats_get(port_id,&port_stats))
		{
			printf("rec_packets :%ld    sen_packets :%ld\n",port_stats.ipackets, port_stats.opackets);
			printf("rec_bytes :%ld      sen_bytes :%ld\n",port_stats.ibytes, port_stats.obytes);
			printf("rec_error :%ld      sen_error :%ld\n",port_stats.ierrors, port_stats.oerrors);
			printf("rec_missed :%ld     rx_nombuf :%ld\n",port_stats.imissed,port_stats.rx_nombuf);
		}
		int time = endtime - starttime;
		printf("-------send total :%d  ------count : %d -----time :%d \n", send_total0, count, time);	
	}	
	return 0;
}

int
main(int argc, char **argv)
{

	int ret;
	uint32_t nb_lcores;
	uint32_t nb_ports;
	unsigned lcoreid;

	uint8_t  nb_rx_queue, nb_tx_queue;
	uint16_t nb_rx_desc, nb_tx_desc;
	
	struct rte_eth_dev_info default_eth_dev_info_before;
	struct rte_eth_dev_info default_eth_dev_info_after;
	struct rte_eth_rxconf default_rxconf;
	struct rte_eth_txconf default_txconf;
	struct rte_eth_desc_lim 	rx_desc_lim;
	struct rte_eth_desc_lim 	tx_desc_lim;
	
	nb_rx_queue = 1;    //�˿ڽ��ն�������
	nb_tx_queue = 1;    //�˿ڴ����������
	nb_rx_desc = 128;   //�˿ڽ��ն�������������
	nb_tx_desc = 512;   //�˿ڴ����������������
	rx_queue_id = 0;    //��ʹ�ý��ն��� 0 
	tx_queue_id = 0;    //��ʹ�ô������ 0 
	port_id = 0;		//��ʹ�ö˿� 0 
	lcore_id = 1;       //��ʹ�õ��߼��� 1
	force_quit = false;

	ret = rte_eal_init(argc, argv);
	if (ret < 0)
		rte_panic("Cannot init EAL\n");
	
	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);
	
	//�˿�����
	nb_ports = rte_eth_dev_count();
	if (nb_ports > RTE_MAX_ETHPORTS)
		nb_ports = RTE_MAX_ETHPORTS;
	//�߼�������
	nb_lcores = rte_lcore_count();
	printf("number of lcores: %d    number of ports: %d\n", nb_lcores, nb_ports);
	//���߼��� CPU ��۱��
	socket_id = rte_lcore_to_socket_id(rte_get_master_lcore());
	
	//�����ڴ��
	char s[64];//�ڴ������
	snprintf(s, sizeof(s), "mbuf_pool_%d", socket_id);
	pktmbuf_pool = rte_pktmbuf_pool_create(s,NB_MBUF, MEMPOOL_CACHE_SIZE, 0,RTE_MBUF_DEFAULT_BUF_SIZE, socket_id);
	if (pktmbuf_pool == NULL)
		rte_exit(EXIT_FAILURE, "Cannot init mbuf pool on socket %d\n", socket_id);
	else
		printf("Allocated mbuf pool on socket %d\n", socket_id);
	
	//��ȡ�˿�mac ��ַ
	rte_eth_macaddr_get(port_id, &src_eth_addrs);
	print_ethaddr("SRC1  Mac Address:", &src_eth_addrs);
	
	rte_eth_macaddr_get(port_id + 2, &eth_addrs);
	print_ethaddr("SRC2  Mac Address:", &eth_addrs);
	
	//Ŀ��mac ��ַ
	void *tmp;
	tmp = &des_eth_addrs.addr_bytes[0];
	//*((uint64_t *)tmp) = (((uint64_t)0x59 << 40) | ((uint64_t)0x41 << 32) | ((uint64_t)0x02 << 24) | ((uint64_t)0x4A << 16) | ((uint64_t)0x53 << 8) | (uint64_t)0x2C);
	//*((uint64_t *)tmp) = (((uint64_t)0x30 << 40) | ((uint64_t)0x05 << 32) | ((uint64_t)0x05 << 24) | ((uint64_t)0x0A << 16) | ((uint64_t)0x11 << 8) | (uint64_t)0x00);
     *((uint64_t *)tmp) = (((uint64_t)0xFF << 40) | ((uint64_t)0xFF << 32) | ((uint64_t)0xFF << 24) | ((uint64_t)0xFF << 16) | ((uint64_t)0xFF << 8) | (uint64_t)0xFF);
	print_ethaddr("DES  Mac Address:", &des_eth_addrs);
	
	//�˿�����
	ret = rte_eth_dev_configure(port_id, nb_rx_queue, nb_tx_queue, &port_conf);
	if (ret < 0)
		rte_exit(EXIT_FAILURE, "Cannot configure device: err=%d, port=%d\n",ret, port_id);
	//���Rx��Tx�������������Ƿ�����������̫���豸��Ϣ�����������ƣ����������Ϊ�߽� nb_rx_desc =128,nb_tx_desc=128
	ret = rte_eth_dev_adjust_nb_rx_tx_desc(port_id, &nb_rx_desc,&nb_tx_desc);
	
	//��ȡ�˿�Ĭ��������Ϣ
	rte_eth_dev_info_get(port_id, &default_eth_dev_info_before);
	
	
	//�˿� TX ��������
	fflush(stdout);
	
	default_txconf = default_eth_dev_info_before.default_txconf;
	tx_desc_lim = default_eth_dev_info_before.tx_desc_lim;
	printf("config before ---- tx_free_thresh : %d ,desc_max ��%d ,desc_min : %d \n",default_txconf.tx_free_thresh, tx_desc_lim.nb_max, tx_desc_lim.nb_min);
	
	default_txconf.tx_free_thresh = (uint16_t) MAX_PKT_BURST;
	ret = rte_eth_tx_queue_setup(port_id, tx_queue_id, nb_tx_desc, socket_id, NULL);
	if (ret < 0)
		rte_exit(EXIT_FAILURE, "rte_eth_tx_queue_setup: err=%d, port=%d\n", ret, port_id);
		
	
	//�˿� RX ��������
	fflush(stdout);
	
	default_rxconf = default_eth_dev_info_before.default_rxconf;
	rx_desc_lim = default_eth_dev_info_before.rx_desc_lim;
	printf("config before ---- rx_free_thresh : %d ,desc_max ��%d ,desc_min : %d \n",default_rxconf.rx_free_thresh, rx_desc_lim.nb_max, rx_desc_lim.nb_min);
	
	default_rxconf.rx_free_thresh = (uint16_t) MAX_PKT_BURST;
	ret = rte_eth_rx_queue_setup(port_id, rx_queue_id, nb_rx_desc, socket_id, NULL, pktmbuf_pool);
	if (ret < 0)
		rte_exit(EXIT_FAILURE, "rte_eth_rx_queue_setup: err=%d,port=%d\n", ret, port_id);
	
	rte_delay_ms(5000);//�ӳ�5��
	memset(&default_txconf, 0, sizeof(default_txconf));
	memset(&default_rxconf, 0, sizeof(default_rxconf));
	
	memset(&tx_desc_lim, 0, sizeof(tx_desc_lim));
	memset(&rx_desc_lim, 0, sizeof(rx_desc_lim));
	
	//��ȡ�˿�Ĭ��������Ϣ
	rte_eth_dev_info_get(port_id, &default_eth_dev_info_after);
	
	default_txconf = default_eth_dev_info_after.default_txconf;
	tx_desc_lim = default_eth_dev_info_after.tx_desc_lim;
	printf("config after  ---- tx_free_thresh : %d ,desc_max ��%d ,desc_min : %d \n",default_txconf.tx_free_thresh, tx_desc_lim.nb_max, tx_desc_lim.nb_min);
	default_rxconf = default_eth_dev_info_after.default_rxconf;
	rx_desc_lim = default_eth_dev_info_after.rx_desc_lim;
	printf("config after  ---- rx_free_thresh : %d ,desc_max ��%d ,desc_min : %d \n",default_rxconf.rx_free_thresh, rx_desc_lim.nb_max, rx_desc_lim.nb_min);
	
	
	
	/*�����˿����� */
	ret = rte_eth_dev_start(port_id);
	if (ret < 0)
		rte_exit(EXIT_FAILURE, "rte_eth_dev_start: err=%d, port=%d\n",ret, port_id);

	printf("started: Port %d\n", port_id);
	
	/* ���ö˿���������ģʽ */
    //rte_eth_promiscuous_enable(port_id);
	
	/*�ȴ����������ɹ�*/
	#define CHECK_INTERVAL 100 /* 100ms */	
	#define MAX_CHECK_TIME 50 /* 5s (50 * 100ms) in total */
	uint8_t count;
	struct rte_eth_link link;
	for (count = 0; count <= MAX_CHECK_TIME; count++) {
		if (force_quit)
			return 0;
		memset(&link, 0, sizeof(link));
		rte_eth_link_get_nowait(port_id, &link);
		if (link.link_status)
			printf("Port %d Link Up - speed %u Mbps - %s\n", (uint8_t)port_id,(unsigned)link.link_speed,
					(link.link_duplex == ETH_LINK_FULL_DUPLEX) ?
						("full-duplex") : ("half-duplex\n"));
		else
			printf("Port %d Link Down\n",(uint8_t)port_id);
		rte_delay_ms(CHECK_INTERVAL);
	}
	printf("�����߼���ִ������\n");
	/*�����߼���ִ������*/
	rte_eal_mp_remote_launch(app_lcore_main_loop, NULL, CALL_MASTER);
	
	/*�ȴ��߼����˳�*/
	RTE_LCORE_FOREACH_SLAVE(lcoreid) {
		if (rte_eal_wait_lcore(lcoreid) < 0) {
			return -1;
		}
	}
	printf("Bye...\n");
	printf("Closing port %d...\n", port_id);
	
	/*ֹͣ�˿�����*/
	rte_eth_dev_stop(port_id);
	/*�رն˿�����*/
	rte_eth_dev_close(port_id);
	
	return 0;
}

