#include <net/net.h>
#include <uart/uart.h>

#define MAX_SOCK_NUM  32

static struct socket *g_sock_fds[MAX_SOCK_NUM];

static inline struct socket *get_sock(int fd)
{
	if (fd <= 0 || fd >= MAX_SOCK_NUM)
		return NULL;

	return g_sock_fds[fd];
}

#define PORT_MIN 50000

static inline __u16 port_alloc(int type)
{
	static __u16 port = PORT_MIN;

	port++;
	if (port < PORT_MIN)
		port = PORT_MIN;

	return port;
}

static inline void port_free(__u16 port)
{
}

static int tcp_wait_for_state(const struct socket *sock, enum tcp_state state)
{
	int to;

	for (to = 0; to < 100; to++)
	{
		netif_rx_poll();
		if (sock->state == state)
			return 0;

		mdelay(100);
	}

	return -ETIMEDOUT;
}

int socket(int domain, int type, int protocol)
{
	int fd;
	struct socket *sock;

	for (fd = 1; fd < MAX_SOCK_NUM; fd++)
	{
		if (NULL == g_sock_fds[fd])
			goto alloc_sock;
	}

	return -ENODEV;

alloc_sock:
	sock = malloc(sizeof(struct socket));

	if (NULL == sock)
	{
		DPRINT("%s: fail to alloc socket!\n", __func__);
		return -ENOMEM;
	}

	sock->type = type;
	sock->state = TCPS_CLOSED;
	sock->seq_num = 1; // fixme
	sock->ack_num = 0;
	memset(sock->saddr, 0, sizeof(sock->saddr));
	list_head_init(&sock->tx_qu);
	list_head_init(&sock->rx_qu);

	g_sock_fds[fd] = sock;

	return fd;
}

static void free_skb_list(struct list_node *qu)
{
	struct list_node *first;
	struct sock_buff *skb;

	while (!list_is_empty(qu))
	{
		first = qu->next;

		list_del_node(first);
		skb = container_of(first, struct sock_buff, node);
		skb_free(skb);
	}
}

int sk_close(int fd)
{
	int ret;
	__u32 psr;
	struct socket *sock;
	struct sock_buff *skb;

	sock = get_sock(fd);
	if (NULL == sock)
		return -EINVAL;

	if (TCPS_ESTABLISHED == sock->state || \
		TCPS_SYN_RCVD == sock->state || \
		TCPS_CLOSE_WAIT == sock->state)
	{
		enum tcp_state last_state;

		skb = skb_alloc(ETH_HDR_LEN + IP_HDR_LEN + TCP_HDR_LEN, 0);
		// if null
		skb->sock = sock;

		lock_irq_psr(psr);
		tcp_send_packet(skb, FLG_FIN | FLG_ACK, NULL);
		if (TCPS_CLOSE_WAIT == sock->state)
			sock->state = TCPS_LAST_ACK;
		else
			sock->state = TCPS_FIN_WAIT1;
		last_state = sock->state;
		unlock_irq_psr(psr);

		if (TCPS_LAST_ACK == last_state)
			ret = tcp_wait_for_state(sock, TCPS_CLOSED);
		else
			ret = tcp_wait_for_state(sock, TCPS_TIME_WAIT);

		if (ret < 0)
			return ret;
	}

	// fixme
	if (TCPS_TIME_WAIT == sock->state)
	{
		ret = tcp_wait_for_state(sock, TCPS_CLOSED);
		if (ret < 0)
			sock->state = TCPS_CLOSED;
	}

	if (TCPS_CLOSED == sock->state)
	{
		lock_irq_psr(psr);
		free_skb_list(&sock->rx_qu);
		free_skb_list(&sock->tx_qu);
		free(sock);
		unlock_irq_psr(psr);
		g_sock_fds[fd] = NULL;
	}
	else
	{
		printf("%s(): Warning! (state = %d)\n", __func__, sock->state);
		return -EINVAL;
	}

	return 0;
}

struct eth_addr *gethostaddr(const u32 nip)
{
	u32 count = 0;
	struct eth_addr *remote_addr;

	arp_send_packet((u8 *)&nip, NULL, ARP_OP_REQ);

	while (count < 100000)
	{
		int ret;
		u8  key;

		ret = uart_read(CONFIG_DBGU_ID, &key, 1, WAIT_ASYNC);
		if (ret > 0 && key == CHAR_CTRL_C)
		{
			return NULL;
		}

		netif_rx_poll();

		remote_addr = getaddr(nip);
		if (remote_addr)
			break;

		// TODO: add re-send code here
		udelay(3);
		count++;
	}

	return remote_addr;
}

int bind(int fd, const struct sockaddr *addr, socklen_t len)
{
	int ret = 0;
	struct socket *sock;
	const struct sockaddr_in *sa;
	struct sockaddr_in *sin;

	sock = get_sock(fd);
	if (NULL == sock)
	{
		// printf
		return -EINVAL;
	}

	sin = &sock->saddr[SA_SRC];

	sin->sin_family = addr->sa_family;

	switch (addr->sa_family)
	{
	case AF_INET:
	default: // fixme: move default down
		sa = (const struct sockaddr_in *)addr;

		sin->sin_port = sa->sin_port ? sa->sin_port : htons(port_alloc(sock->type));

		if (sa->sin_addr.s_addr == htonl(INADDR_ANY))
			ret = ndev_ioctl(NULL, NIOC_GET_IP, &sin->sin_addr.s_addr);
		else
			sin->sin_addr = sa->sin_addr;

		break;
	}

	return ret;
}

//
ssize_t sendto(int fd, const void *buff, u32 buff_size, int flags,
			const struct sockaddr *dest_addr, socklen_t addr_size)
{
	struct socket *sock;
	struct sock_buff *skb;

	sock = get_sock(fd);

	if (NULL == sock)
	{
		// printf
		return -EINVAL;
	}

	memcpy(&sock->saddr[SA_DST], dest_addr, addr_size);

	skb = skb_alloc(ETH_HDR_LEN + IP_HDR_LEN + UDP_HDR_LEN, buff_size);
	// if null

	skb->sock = sock;

	memcpy(skb->data, buff, buff_size);

	udp_send_packet(skb);

	return buff_size;
}

long recvfrom(int fd, void *buf, u32 n, int flags,
		struct sockaddr *src_addr, socklen_t *addrlen)
{
	struct socket *sock;
	struct sock_buff *skb;
	u32 pkt_len;

	sock = get_sock(fd);

	if (NULL == sock)
	{
		return -EINVAL;
	}

	skb = udp_recv_packet(sock);
	if(NULL == skb)
		return 0;

	// fixme !
	pkt_len   = skb->size <= n ? skb->size : n;

	*addrlen = sizeof(struct sockaddr_in);
	memcpy(src_addr, &sock->saddr[SA_DST], *addrlen);

	memcpy(buf, skb->data, pkt_len);

	skb_free(skb);

	return pkt_len;
}

#if 0
static void tcp_make_option(u8 *opt, u16 size)
{
	__u32 tsv = htonl(3455994), tser = 0;

	if (size > 0)
	{
	memset(opt, 0, size);

#if 1
	// MSS
	*opt++ = 2;
	*opt++ = 4;
	*(__u16 *)opt = htons(1460);
	opt += 2;
#endif

#if 1
	// SACK
	*opt++ = 4;
	*opt++ = 2;
#endif

#if 1
	// Time stamp
	*opt++ = 8;
	*opt++ = 10;
	memcpy(opt, &tsv, 4);
	opt += 4;
	memcpy(opt, &tser, 4);
	opt += 4;
#endif

#if 1
	// NOP
	*opt++ = 1;
#endif

#if 1
	// WS
	*opt++ = 3;
	*opt++ = 3;
	*opt++ = 5;
#endif
	}
}
#endif

int connect(int fd, const struct sockaddr *addr, socklen_t len)
{
	int ret;
	__u32 psr;
	struct socket *sock;
	struct sock_buff *skb;

	sock = get_sock(fd);
	if (NULL == sock)
	{
		// DPRINT
		return -ENOENT;
	}

	memcpy(&sock->saddr[SA_DST], addr, len);

	// SYN
	skb = skb_alloc(ETH_HDR_LEN + IP_HDR_LEN + TCP_HDR_LEN, 0);
	// if null
	skb->sock = sock;

	lock_irq_psr(psr);
	tcp_send_packet(skb, FLG_SYN, NULL);
	sock->state = TCPS_SYN_SENT;
	unlock_irq_psr(psr);

	ret = tcp_wait_for_state(sock, TCPS_ESTABLISHED);

	if (ret < 0)
		sock->state = TCPS_CLOSED;

	return ret;
}

ssize_t send(int fd, const void *buf, size_t n, int flag)
{
	struct socket *sock;
	struct sock_buff *skb;

	sock = get_sock(fd);
	if (NULL == sock)
		return -EINVAL;

	if (TCPS_ESTABLISHED != sock->state)
		return -EIO;

	skb = skb_alloc(ETH_HDR_LEN + IP_HDR_LEN + TCP_HDR_LEN, n);
	// if null
	skb->sock = sock;
	memcpy(skb->data, buf, n);

	tcp_send_packet(skb, FLG_PSH | FLG_ACK, NULL);

#if 0
	for (time_out = 0; time_out < 10; time_out++)
	{
		netif_rx_poll();
		if (sock->state == 1)
			break;

		mdelay(100);
		printf("%s() line %d\n", __func__, __LINE__);
	}
#endif

	return n;
}

ssize_t recv(int fd, void *buf, size_t n, int flag)
{
	ssize_t pkt_len;
	struct socket *sock;
	struct sock_buff *skb;

	sock = get_sock(fd);
	if (NULL == sock)
		return -ENOENT;

	if (TCPS_ESTABLISHED != sock->state)
		return -EIO;

	skb = tcp_recv_packet(sock);
	if (skb == NULL)
	{
		return -EIO;
	}

	pkt_len = skb->size <= n ? skb->size : n;
	memcpy(buf, skb->data, pkt_len);

	skb_free(skb);

	return pkt_len;
}

struct socket *tcp_search_socket(const struct tcp_header *tcp_pkt, const struct ip_header *ip_pkt)
{
	int fd;
	struct socket *sock;
	struct sockaddr_in *saddr;

	for (fd = 1; fd < MAX_SOCK_NUM; fd++)
	{
		sock = g_sock_fds[fd];

		if (NULL == sock)
			continue;

		saddr = &sock->saddr[SA_SRC];

		if (sock->type != SOCK_STREAM)
			continue;

#if 0
		if (memcmp(saddr->sin_addr, ip_pkt->src_ip, 4) != 0)
		{
			printf("sock = 0x%x, des ip is %d (NOT %d.%d.%d.%d, %02x.%02x.%02x.%02x.%02x.%02x)\n",
				saddr,
				ip_pkt->src_ip[3],
				saddr->des_ip[0],
				saddr->des_ip[1],
				saddr->des_ip[2],
				saddr->des_ip[3],
				saddr->des_mac[0],
				saddr->des_mac[1],
				saddr->des_mac[2],
				saddr->des_mac[3],
				saddr->des_mac[4],
				saddr->des_mac[5]
				);
			continue;
		}
#endif

		if (saddr->sin_port != tcp_pkt->dst_port)
		{
#if 0
			printf("from %d, src port: 0x%x != 0x%x\n", ip_pkt->src_ip[3], BE16_TO_CPU(saddr->src_port), BE16_TO_CPU(tcp_pkt->dst_port));
#endif
			continue;
		}

#if 0
		if (memcmp(&src_ip, ip_pkt->des_ip, 4) != 0)
		{
			printf("src ip: %d.%d.%d.%d\n",
				ip_pkt->des_ip[0],
				ip_pkt->des_ip[1],
				ip_pkt->des_ip[2],
				ip_pkt->des_ip[3]
				);

			continue;
		}
#endif

#if 0
		memcmp(&sock->saddr[SA_DST].sin_addr, ip_hdr->src_ip, IPV4_ADR_LEN);
		memcmp(sock->saddr[SA_DST].sin_port = tcp_hdr->src_port);
#endif

		return sock;
	}

	return NULL;
}

struct socket *udp_search_socket(const struct udp_header *udp_pkt, const struct ip_header *ip_pkt)
{
	int fd;
	struct socket *sock;
	struct sockaddr_in *saddr;
	u32 src_ip;

	ndev_ioctl(NULL, NIOC_GET_IP, &src_ip);

	for (fd = 1; fd < MAX_SOCK_NUM; fd++)
	{
		sock = g_sock_fds[fd];

		if (NULL == sock)
			continue;

		saddr = &sock->saddr[SA_SRC];

		if (sock->type != SOCK_DGRAM)
			continue;

#if 0
		if (memcmp(saddr->sin_addr, ip_pkt->src_ip, 4) != 0)
		{
			printf("sock = 0x%x, des ip is %d (NOT %d.%d.%d.%d, %02x.%02x.%02x.%02x.%02x.%02x)\n",
				saddr,
				ip_pkt->src_ip[3],
				saddr->des_ip[0],
				saddr->des_ip[1],
				saddr->des_ip[2],
				saddr->des_ip[3],
				saddr->des_mac[0],
				saddr->des_mac[1],
				saddr->des_mac[2],
				saddr->des_mac[3],
				saddr->des_mac[4],
				saddr->des_mac[5]
				);
			continue;
		}
#endif

		if (saddr->sin_port != udp_pkt->dst_port)
		{
#if 0
			printf("from %d, src port: 0x%x != 0x%x\n", ip_pkt->src_ip[3], BE16_TO_CPU(saddr->src_port), BE16_TO_CPU(udp_pkt->dst_port));
#endif
			continue;
		}

#if 0
		if (memcmp(&src_ip, ip_pkt->des_ip, 4) != 0)
		{
			printf("src ip: %d.%d.%d.%d\n",
				ip_pkt->des_ip[0],
				ip_pkt->des_ip[1],
				ip_pkt->des_ip[2],
				ip_pkt->des_ip[3]
				);

			continue;
		}
#endif

		return sock;
	}

	return NULL;
}

void socket_init(void)
{
	int fd;

	for (fd = 0; fd < MAX_SOCK_NUM; fd++)
		g_sock_fds[fd] = NULL;
}
