/*
 * LKL network related utilities
 *
 * Picked from a code of lkl-linux-2.6
 * https://github.com/lkl/lkl-linux-2.6/blob/master/arch/lkl/envs/lib/net.c
 *
 * Author: Octavian Purdila
 *         Lucian Adrian Grijincu
 *         Hajime Tazaki <thehajime@gmail.com>
 *
 */

#include <lkl_host.h>

#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <linux/socket.h>
#include <sys/socket.h>
typedef uint64_t __u64;
typedef __u64 __be64;
typedef __u64 __le64;
typedef uint32_t __u32;
typedef __u32 __be32;
typedef __u32 __le32;
typedef uint16_t __u16;
typedef __u16 __be16;
typedef __u16 __le16;

#include <netinet/in.h>
#include <linux/if.h>
#include <linux/sockios.h>
#include <linux/route.h>



#define htonl(x) __cpu_to_be32(x)
#define htons(x) __cpu_to_be16(x)
#define ntohl(x) __be32_to_cpu(x)

static inline void
set_sockaddr(struct sockaddr_in *sin, unsigned int addr, unsigned short port)
{
	sin->sin_family = AF_INET;
	sin->sin_addr.s_addr = addr;
	sin->sin_port = port;
}

static inline int ifindex_to_name(int sock, struct ifreq *ifr, int ifindex)
{
	ifr->ifr_ifindex = ifindex;
	return lkl_sys_ioctl(sock, SIOCGIFNAME, (long)ifr);
}

int lkl_if_up(int ifindex)
{
	struct ifreq ifr;
	int err, sock = lkl_sys_socket(PF_INET, SOCK_DGRAM, 0);

	if (sock < 0)
		return sock;
	err = ifindex_to_name(sock, &ifr, ifindex);
	if (err < 0)
		return err;

	err = lkl_sys_ioctl(sock, SIOCGIFFLAGS, (long)&ifr);
	if (!err) {
		ifr.ifr_flags |= IFF_UP;
		err = lkl_sys_ioctl(sock, SIOCSIFFLAGS, (long)&ifr);
	}

	lkl_sys_close(sock);

	return err;
}

int lkl_if_down(int ifindex)
{
	struct ifreq ifr;
	int err, sock = lkl_sys_socket(PF_INET, SOCK_DGRAM, 0);

	if (sock < 0)
		return sock;

	err = ifindex_to_name(sock, &ifr, ifindex);
	if (err < 0)
		return err;

	err = lkl_sys_ioctl(sock, SIOCGIFFLAGS, (long)&ifr);
	if (!err) {
		ifr.ifr_flags &= ~IFF_UP;
		err = lkl_sys_ioctl(sock, SIOCSIFFLAGS, (long)&ifr);
	}

	lkl_sys_close(sock);

	return err;
}

int lkl_if_set_ipv4(int ifindex, unsigned int addr, unsigned int netmask_len)
{
	struct ifreq ifr;
	struct sockaddr_in *sin = (struct sockaddr_in *)&ifr.ifr_addr;
	int err, sock = lkl_sys_socket(PF_INET, SOCK_DGRAM, 0);

	if (sock < 0)
		return sock;

	err = ifindex_to_name(sock, &ifr, ifindex);
	if (err < 0)
		return err;

	if (netmask_len >= 31)
		return -EINVAL;

	set_sockaddr(sin, addr, 0);
	err = lkl_sys_ioctl(sock, SIOCSIFADDR, (long)&ifr);
	if (!err) {
		int netmask = (((1<<netmask_len)-1))<<(32-netmask_len);

		set_sockaddr(sin, htonl(netmask), 0);
		err = lkl_sys_ioctl(sock, SIOCSIFNETMASK, (long)&ifr);
		if (!err) {
			set_sockaddr(sin, htonl(ntohl(addr)|~netmask), 0);
			err = lkl_sys_ioctl(sock, SIOCSIFBRDADDR, (long)&ifr);
		}
	}

	lkl_sys_close(sock);

	return err;
}


int lkl_set_gateway(unsigned int addr)
{
	struct rtentry re;
	int err, sock = lkl_sys_socket(PF_INET, SOCK_DGRAM, 0);

	if (sock < 0)
		return sock;

	memset(&re, 0, sizeof(re));
	set_sockaddr((struct sockaddr_in *) &re.rt_dst, 0, 0);
	set_sockaddr((struct sockaddr_in *) &re.rt_genmask, 0, 0);
	set_sockaddr((struct sockaddr_in *) &re.rt_gateway, addr, 0);
	re.rt_flags = RTF_UP | RTF_GATEWAY;
	err = lkl_sys_ioctl(sock, SIOCADDRT, (long)&re);
	lkl_sys_close(sock);

	return err;
}
