#include "tracer.h"

#include "tracer-events.h"
#include "tracer-maps.h"
#include "tracer-stats.h"
#include "tracer-telemetry.h"
#include "bpf_helpers.h"
#include "bpf_endian.h"
#include "ip.h"
#include "netns.h"
#include "sockfd.h"
#include "conn-tuple.h"

#ifdef FEATURE_IPV6_ENABLED
#include "ipv6.h"
#endif

#include <linux/kconfig.h>
#include <linux/version.h>
#include <net/inet_sock.h>
#include <net/net_namespace.h>
#include <net/route.h>
#include <net/tcp_states.h>
#include <uapi/linux/ip.h>
#include <uapi/linux/ipv6.h>
#include <uapi/linux/ptrace.h>
#include <linux/tcp.h>
#include <uapi/linux/udp.h>

#ifndef LINUX_VERSION_CODE
# error "kernel version not included?"
#endif

static __always_inline __be32 rt_nexthop_bpf(struct rtable *rt) {
    if (!rt) {
        return 0;
    }
    __be32 hop = 0;
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 2, 0)
    bpf_probe_read(&hop, sizeof(hop), &rt->rt_gateway);
#else
    u8 family;
    bpf_probe_read(&family, sizeof(family), &rt->rt_gw_family);
    if (family == AF_INET) {
        bpf_probe_read(&hop, sizeof(hop), &rt->rt_gw4);
    }
#endif
    return hop;
}

static __always_inline void handle_tcp_stats(conn_tuple_t* t, struct sock* skp) {
    __u32 rtt = 0;
    __u32 rtt_var = 0;
    bpf_probe_read(&rtt, sizeof(rtt), &tcp_sk(skp)->srtt_us);
    bpf_probe_read(&rtt_var, sizeof(rtt_var), &tcp_sk(skp)->mdev_us);


    tcp_stats_t stats = { .retransmits = 0, .rtt = rtt, .rtt_var = rtt_var };
    update_tcp_stats(t, stats);
}

static __always_inline void get_tcp_segment_counts(struct sock* skp, __u32* packets_in, __u32* packets_out) {
    bpf_probe_read(packets_out, sizeof(*packets_out), &tcp_sk(skp)->segs_out);
    bpf_probe_read(packets_in, sizeof(*packets_in), &tcp_sk(skp)->segs_in);
}

SEC("kprobe/tcp_sendmsg")
int kprobe__tcp_sendmsg(struct pt_regs* ctx) {
    __u32 packets_in = 0;
    __u32 packets_out = 0;
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 1, 0)
    struct sock* skp = (struct sock*)PT_REGS_PARM2(ctx);
    size_t size = (size_t)PT_REGS_PARM4(ctx);
#else
    struct sock* skp = (struct sock*)PT_REGS_PARM1(ctx);
    size_t size = (size_t)PT_REGS_PARM3(ctx);
#endif
    u64 pid_tgid = bpf_get_current_pid_tgid();
    log_debug("kprobe/tcp_sendmsg: size: %d\n", size);

    conn_tuple_t t = {};
    if (!read_conn_tuple(&t, skp, pid_tgid, CONN_TYPE_TCP)) {
        return 0;
    }

    handle_tcp_stats(&t, skp);
    get_tcp_segment_counts(skp, &packets_in, &packets_out);
    return handle_message(&t, size, 0, CONN_DIRECTION_UNKNOWN, packets_out, packets_in, PACKET_COUNT_ABSOLUTE);
}

SEC("kprobe/tcp_cleanup_rbuf")
int kprobe__tcp_cleanup_rbuf(struct pt_regs* ctx) {
    __u32 packets_in = 0;
    __u32 packets_out = 0;
    struct sock* sk = (struct sock*)PT_REGS_PARM1(ctx);
    int copied = (int)PT_REGS_PARM2(ctx);
    if (copied < 0) {
        return 0;
    }
    u64 pid_tgid = bpf_get_current_pid_tgid();
    log_debug("kprobe/tcp_cleanup_rbuf: pid_tgid: %d, copied: %d\n", pid_tgid, copied);

    bpf_probe_read(&packets_out, sizeof(packets_out), &tcp_sk(sk)->segs_out);
    bpf_probe_read(&packets_in, sizeof(packets_in), &tcp_sk(sk)->segs_in);
    conn_tuple_t t = {};
    if (!read_conn_tuple(&t, sk, pid_tgid, CONN_TYPE_TCP)) {
        return 0;
    }

    return handle_message(&t, 0, copied, CONN_DIRECTION_UNKNOWN, packets_out, packets_in, PACKET_COUNT_ABSOLUTE);
}

SEC("kprobe/tcp_close")
int kprobe__tcp_close(struct pt_regs* ctx) {
    struct sock* sk;
    conn_tuple_t t = {};
    u64 pid_tgid = bpf_get_current_pid_tgid();
    sk = (struct sock*)PT_REGS_PARM1(ctx);

    clear_sockfd_maps(sk);

    // Get network namespace id
    log_debug("kprobe/tcp_close: tgid: %u, pid: %u\n", pid_tgid >> 32, pid_tgid & 0xFFFFFFFF);
    if (!read_conn_tuple(&t, sk, pid_tgid, CONN_TYPE_TCP)) {
        return 0;
    }
    log_debug("kprobe/tcp_close: netns: %u, sport: %u, dport: %u\n", t.netns, t.sport, t.dport);

    cleanup_conn(&t);
    return 0;
}

SEC("kretprobe/tcp_close")
int kretprobe__tcp_close(struct pt_regs* ctx) {
    flush_conn_close_if_full(ctx);
    return 0;
}

#ifdef FEATURE_IPV6_ENABLED
SEC("kprobe/ip6_make_skb")
int kprobe__ip6_make_skb(struct pt_regs* ctx) {
    struct sock* sk = (struct sock*)PT_REGS_PARM1(ctx);
    size_t size = (size_t)PT_REGS_PARM4(ctx);
    u64 pid_tgid = bpf_get_current_pid_tgid();
    size = size - sizeof(struct udphdr);

    conn_tuple_t t = {};
    if (!read_conn_tuple(&t, sk, pid_tgid, CONN_TYPE_UDP)) {
// commit: https://github.com/torvalds/linux/commit/26879da58711aa604a1b866cbeedd7e0f78f90ad
// changed the arguments to ip6_make_skb and introduced the struct ipcm6_cookie
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 7, 0)
        struct flowi6* fl6 = (struct flowi6*)PT_REGS_PARM7(ctx);
#else
        struct flowi6* fl6 = (struct flowi6*)PT_REGS_PARM9(ctx);
#endif
        read_in6_addr(&t.saddr_h, &t.saddr_l, &fl6->saddr);
        read_in6_addr(&t.daddr_h, &t.daddr_l, &fl6->daddr);

        if (!(t.saddr_h || t.saddr_l)) {
            log_debug("ERR(fl6): src addr not set src_l:%d,src_h:%d\n", t.saddr_l, t.saddr_h);
            increment_telemetry_count(udp_send_missed);
            return 0;
        }
        if (!(t.daddr_h || t.daddr_l)) {
            log_debug("ERR(fl6): dst addr not set dst_l:%d,dst_h:%d\n", t.daddr_l, t.daddr_h);
            increment_telemetry_count(udp_send_missed);
            return 0;
        }

        // Check if we can map IPv6 to IPv4
        if (is_ipv4_mapped_ipv6(t.saddr_h, t.saddr_l, t.daddr_h, t.daddr_l)) {
            t.metadata |= CONN_V4;
            t.saddr_h = 0;
            t.daddr_h = 0;
            t.saddr_l = (u32)(t.saddr_l >> 32);
            t.daddr_l = (u32)(t.daddr_l >> 32);
        } else {
            t.metadata |= CONN_V6;
        }

        bpf_probe_read(&t.sport, sizeof(t.sport), &fl6->fl6_sport);
        bpf_probe_read(&t.dport, sizeof(t.dport), &fl6->fl6_dport);

        if (t.sport == 0 || t.dport == 0) {
            log_debug("ERR(fl6): src/dst port not set: src:%d, dst:%d\n", t.sport, t.dport);
            increment_telemetry_count(udp_send_missed);
            return 0;
        }

        t.sport = bpf_ntohs(t.sport);
        t.dport = bpf_ntohs(t.dport);
    }

    log_debug("kprobe/ip6_make_skb: pid_tgid: %d, size: %d\n", pid_tgid, size);
    handle_message(&t, size, 0, CONN_DIRECTION_UNKNOWN, 1, 0, PACKET_COUNT_INCREMENT);
    increment_telemetry_count(udp_send_processed);

    return 0;
}
#endif

// Note: This is used only in the UDP send path.
SEC("kprobe/ip_make_skb")
int kprobe__ip_make_skb(struct pt_regs* ctx) {
    struct sock* sk = (struct sock*)PT_REGS_PARM1(ctx);
    size_t size = (size_t)PT_REGS_PARM5(ctx);
    u64 pid_tgid = bpf_get_current_pid_tgid();

    size = size - sizeof(struct udphdr);

    conn_tuple_t t = {};
    if (!read_conn_tuple(&t, sk, pid_tgid, CONN_TYPE_UDP)) {
        struct flowi4* fl4 = (struct flowi4*)PT_REGS_PARM2(ctx);
        bpf_probe_read(&t.saddr_l, sizeof(__be32), &fl4->saddr);
        bpf_probe_read(&t.daddr_l, sizeof(__be32), &fl4->daddr);
        if (!t.saddr_l || !t.daddr_l) {
            log_debug("ERR(fl4): src/dst addr not set src:%d,dst:%d\n", t.saddr_l, t.daddr_l);
            increment_telemetry_count(udp_send_missed);
            return 0;
        }

        bpf_probe_read(&t.sport, sizeof(t.sport), &fl4->fl4_sport);
        bpf_probe_read(&t.dport, sizeof(t.dport), &fl4->fl4_dport);
        t.sport = bpf_ntohs(t.sport);
        t.dport = bpf_ntohs(t.dport);
        if (t.sport == 0 || t.dport == 0) {
            log_debug("ERR(fl4): src/dst port not set: src:%d, dst:%d\n", t.sport, t.dport);
            increment_telemetry_count(udp_send_missed);
            return 0;
        }
    }

    log_debug("kprobe/ip_send_skb: pid_tgid: %d, size: %d\n", pid_tgid, size);
    handle_message(&t, size, 0, CONN_DIRECTION_UNKNOWN, 1, 0, PACKET_COUNT_INCREMENT);
    increment_telemetry_count(udp_send_processed);

    return 0;
}

// We can only get the accurate number of copied bytes from the return value, so we pass our
// sock* pointer from the kprobe to the kretprobe via a map (udp_recv_sock) to get all required info
//
// The same issue exists for TCP, but we can conveniently use the downstream function tcp_cleanup_rbuf
//
// On UDP side, no similar function exists in all kernel versions, though we may be able to use something like
// skb_consume_udp (v4.10+, https://elixir.bootlin.com/linux/v4.10/source/net/ipv4/udp.c#L1500)
SEC("kprobe/udp_recvmsg")
int kprobe__udp_recvmsg(struct pt_regs* ctx) {
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 1, 0)
    struct sock* sk = (struct sock*)PT_REGS_PARM2(ctx);
    struct msghdr* msg = (struct msghdr*)PT_REGS_PARM3(ctx);
    int flags = (int)PT_REGS_PARM6(ctx);
#else
    struct sock* sk = (struct sock*)PT_REGS_PARM1(ctx);
    struct msghdr* msg = (struct msghdr*)PT_REGS_PARM2(ctx);
    int flags = (int)PT_REGS_PARM5(ctx);
#endif
    log_debug("kprobe/udp_recvmsg: flags: %x\n", flags);
    if (flags & MSG_PEEK) {
        return 0;
    }

    u64 pid_tgid = bpf_get_current_pid_tgid();
    udp_recv_sock_t t = { .sk = NULL, .msg = NULL };
    if (sk) {
        bpf_probe_read(&t.sk, sizeof(t.sk), &sk);
    }
    if (msg) {
        bpf_probe_read(&t.msg, sizeof(t.msg), &msg);
    }

    bpf_map_update_elem(&udp_recv_sock, &pid_tgid, &t, BPF_ANY);
    return 0;
}

SEC("kretprobe/udp_recvmsg")
int kretprobe__udp_recvmsg(struct pt_regs* ctx) {
    u64 pid_tgid = bpf_get_current_pid_tgid();

    // Retrieve socket pointer from kprobe via pid/tgid
    udp_recv_sock_t* st = bpf_map_lookup_elem(&udp_recv_sock, &pid_tgid);
    if (!st) { // Missed entry
        return 0;
    }

    // Make sure we clean up the key
    bpf_map_delete_elem(&udp_recv_sock, &pid_tgid);

    int copied = (int)PT_REGS_RC(ctx);
    if (copied < 0) { // Non-zero values are errors (or a peek) (e.g -EINVAL)
        log_debug("kretprobe/udp_recvmsg: ret=%d < 0, pid_tgid=%d\n", copied, pid_tgid);
        return 0;
    }

    log_debug("kretprobe/udp_recvmsg: ret=%d\n", copied);

    struct sockaddr * sa = NULL;
    if (st->msg) {
        bpf_probe_read(&sa, sizeof(sa), &(st->msg->msg_name));
    }

    conn_tuple_t t = {};
    __builtin_memset(&t, 0, sizeof(conn_tuple_t));
    sockaddr_to_addr(sa, &t.daddr_h, &t.daddr_l, &t.dport);

    if (!read_conn_tuple_partial(&t, st->sk, pid_tgid, CONN_TYPE_UDP)) {
        log_debug("ERR(kretprobe/udp_recvmsg): error reading conn tuple, pid_tgid=%d\n", pid_tgid);
        return 0;
    }

    log_debug("kretprobe/udp_recvmsg: pid_tgid: %d, return: %d\n", pid_tgid, copied);
    handle_message(&t, 0, copied, CONN_DIRECTION_UNKNOWN, 0, 1, PACKET_COUNT_INCREMENT);

    return 0;
}

SEC("kprobe/tcp_retransmit_skb")
int kprobe__tcp_retransmit_skb(struct pt_regs* ctx) {
    struct sock* sk = (struct sock*)PT_REGS_PARM1(ctx);

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 7, 0)
    int segs = 1;
#else
    int segs = (int)PT_REGS_PARM3(ctx);
#endif
    log_debug("kprobe/tcp_retransmit\n");

    return handle_retransmit(sk, segs);
}

SEC("kprobe/tcp_set_state")
int kprobe__tcp_set_state(struct pt_regs* ctx) {
    u8 state = (u8)PT_REGS_PARM2(ctx);

    // For now we're tracking only TCP_ESTABLISHED
    if (state != TCP_ESTABLISHED) {
        return 0;
    }

    struct sock* sk = (struct sock*)PT_REGS_PARM1(ctx);
    u64 pid_tgid = bpf_get_current_pid_tgid();
    conn_tuple_t t = {};
    if (!read_conn_tuple(&t, sk, pid_tgid, CONN_TYPE_TCP)) {
        return 0;
    }

    tcp_stats_t stats = { .state_transitions = (1 << state) };
    update_tcp_stats(&t, stats);

    return 0;
}

SEC("kretprobe/inet_csk_accept")
int kretprobe__inet_csk_accept(struct pt_regs* ctx) {

    struct sock* sk = (struct sock*)PT_REGS_RC(ctx);
    if (!sk) {
        return 0;
    }

    u64 pid_tgid = bpf_get_current_pid_tgid();
    log_debug("kretprobe/inet_csk_accept: tgid: %u, pid: %u\n", pid_tgid >> 32, pid_tgid & 0xFFFFFFFF);

    conn_tuple_t t = {};
    if (!read_conn_tuple(&t, sk, pid_tgid, CONN_TYPE_TCP)) {
        return 0;
    }
    handle_tcp_stats(&t, sk);
    handle_message(&t, 0, 0, CONN_DIRECTION_INCOMING, 0, 0, PACKET_COUNT_NONE);

    port_binding_t pb = {};
    pb.netns = t.netns;
    pb.port = t.sport;
    __u8 state = PORT_LISTENING;
    bpf_map_update_elem(&port_bindings, &pb, &state, BPF_NOEXIST);

    log_debug("kretprobe/inet_csk_accept: netns: %u, sport: %u, dport: %u\n", t.netns, t.sport, t.dport);
    return 0;
}

SEC("kprobe/inet_csk_listen_stop")
int kprobe__inet_csk_listen_stop(struct pt_regs* ctx) {
    struct sock* skp = (struct sock*)PT_REGS_PARM1(ctx);
    __u16 lport = read_sport(skp);
    if (lport == 0) {
        log_debug("ERR(inet_csk_listen_stop): lport is 0 \n");
        return 0;
    }

    port_binding_t t = { .netns = 0, .port = 0 };
    t.netns = get_netns(&skp->sk_net);
    t.port = lport;
    bpf_map_delete_elem(&port_bindings, &t);

    log_debug("kprobe/inet_csk_listen_stop: net ns: %u, lport: %u\n", t.netns, t.port);
    return 0;
}

SEC("kprobe/udp_destroy_sock")
int kprobe__udp_destroy_sock(struct pt_regs* ctx) {
    struct sock* skp = (struct sock*)PT_REGS_PARM1(ctx);
    conn_tuple_t tup = {};
    u64 pid_tgid = bpf_get_current_pid_tgid();
    int valid_tuple = read_conn_tuple(&tup, skp, pid_tgid, CONN_TYPE_UDP);

    __u16 lport = 0;
    if (valid_tuple) {
        cleanup_conn(&tup);
        lport = tup.sport;
    } else {
        lport = read_sport(skp);
    }

    if (lport == 0) {
        log_debug("ERR(udp_destroy_sock): lport is 0\n");
        return 0;
    }

    // although we have net ns info, we don't use it in the key
    // since we don't have it everywhere for udp port bindings
    // (see sys_enter_bind/sys_exit_bind below)
    port_binding_t t = {};
    t.netns = 0;
    t.port = lport;
    bpf_map_delete_elem(&udp_port_bindings, &t);

    log_debug("kprobe/udp_destroy_sock: port %d marked as closed\n", lport);

    return 0;
}

SEC("kretprobe/udp_destroy_sock")
int kretprobe__udp_destroy_sock(struct pt_regs * ctx) {
    flush_conn_close_if_full(ctx);
    return 0;
}

//region sys_enter_bind

static __always_inline int sys_enter_bind(struct socket* sock, struct sockaddr* addr) {
    __u64 tid = bpf_get_current_pid_tgid();

    __u16 type = 0;
    bpf_probe_read(&type, sizeof(__u16), &sock->type);
    if ((type & SOCK_DGRAM) == 0) {
        return 0;
    }

    if (addr == NULL) {
        log_debug("sys_enter_bind: could not read sockaddr, sock=%llx, tid=%u\n", sock, tid);
        return 0;
    }

    u16 sin_port = 0;
    sa_family_t family = 0;
    bpf_probe_read(&family, sizeof(sa_family_t), &addr->sa_family);
    if (family == AF_INET) {
        bpf_probe_read(&sin_port, sizeof(u16), &(((struct sockaddr_in*)addr)->sin_port));
    } else if (family == AF_INET6) {
        bpf_probe_read(&sin_port, sizeof(u16), &(((struct sockaddr_in6*)addr)->sin6_port));
    }

    sin_port = bpf_ntohs(sin_port);
    if (sin_port == 0) {
        log_debug("ERR(sys_enter_bind): sin_port is 0\n");
        return 0;
    }

    // write to pending_binds so the retprobe knows we can mark this as binding.
    bind_syscall_args_t args = {};
    args.port = sin_port;

    bpf_map_update_elem(&pending_bind, &tid, &args, BPF_ANY);
    log_debug("sys_enter_bind: started a bind on UDP port=%d sock=%llx tid=%u\n", sin_port, sock, tid);

    return 0;
}

SEC("kprobe/inet_bind")
int kprobe__inet_bind(struct pt_regs* ctx) {
    struct socket *sock = (struct socket*)PT_REGS_PARM1(ctx);
    struct sockaddr* addr = (struct sockaddr*)PT_REGS_PARM2(ctx);
    log_debug("kprobe/inet_bind: sock=%llx, umyaddr=%x\n", sock, addr);
    return sys_enter_bind(sock, addr);
}

SEC("kprobe/inet6_bind")
int kprobe__inet6_bind(struct pt_regs* ctx) {
    struct socket *sock = (struct socket*)PT_REGS_PARM1(ctx);
    struct sockaddr* addr = (struct sockaddr*)PT_REGS_PARM2(ctx);
    log_debug("kprobe/inet6_bind: sock=%llx, umyaddr=%x\n", sock, addr);
    return sys_enter_bind(sock, addr);
}

//endregion

//region sys_exit_bind

static __always_inline int sys_exit_bind(__s64 ret) {
    __u64 tid = bpf_get_current_pid_tgid();

    // bail if this bind() is not the one we're instrumenting
    bind_syscall_args_t* args;
    args = bpf_map_lookup_elem(&pending_bind, &tid);

    log_debug("sys_exit_bind: tid=%u, ret=%d\n", tid, ret);

    if (args == NULL) {
        log_debug("sys_exit_bind: was not a UDP bind, will not process\n");
        return 0;
    }

    bpf_map_delete_elem(&pending_bind, &tid);

    if (ret != 0) {
        return 0;
    }

    __u16 sin_port = args->port;
    __u8 port_state = PORT_LISTENING;
    port_binding_t t = {};
    t.netns = 0; // don't have net ns info in this context
    t.port = sin_port;
    bpf_map_update_elem(&udp_port_bindings, &t, &port_state, BPF_ANY);
    log_debug("sys_exit_bind: bound UDP port %u\n", sin_port);

    return 0;
}

SEC("kretprobe/inet_bind")
int kretprobe__inet_bind(struct pt_regs* ctx) {
    __s64 ret = PT_REGS_RC(ctx);
    log_debug("kretprobe/inet_bind: ret=%d\n", ret);
    return sys_exit_bind(ret);
}

SEC("kretprobe/inet6_bind")
int kretprobe__inet6_bind(struct pt_regs* ctx) {
    __s64 ret = PT_REGS_RC(ctx);
    log_debug("kretprobe/inet6_bind: ret=%d\n", ret);
    return sys_exit_bind(ret);
}

SEC("kprobe/sockfd_lookup_light")
int kprobe__sockfd_lookup_light(struct pt_regs* ctx) {
    int sockfd = (int)PT_REGS_PARM1(ctx);
    u64 pid_tgid = bpf_get_current_pid_tgid();

    // Check if have already a map entry for this pid_fd_t
    // TODO: This lookup eliminates *4* map operations for existing entries
    // but can reduce the accuracy of programs relying on socket FDs for
    // processes with a lot of FD churn
    pid_fd_t key = {
        .pid = pid_tgid >> 32,
        .fd = sockfd,
    };
    struct sock** sock = bpf_map_lookup_elem(&sock_by_pid_fd, &key);
    if (sock != NULL) {
        return 0;
    }

    bpf_map_update_elem(&sockfd_lookup_args, &pid_tgid, &sockfd, BPF_ANY);
    return 0;
}

// this kretprobe is essentially creating:
// * an index of pid_fd_t to a struct sock*;
// * an index of struct sock* to pid_fd_t;
SEC("kretprobe/sockfd_lookup_light")
int kretprobe__sockfd_lookup_light(struct pt_regs* ctx) {
    u64 pid_tgid = bpf_get_current_pid_tgid();
    int *sockfd = bpf_map_lookup_elem(&sockfd_lookup_args, &pid_tgid);
    if (sockfd == NULL) {
        return 0;
    }

    // For now let's only store information for TCP sockets
    struct socket* socket = (struct socket*)PT_REGS_RC(ctx);
    enum sock_type sock_type = 0;
    bpf_probe_read(&sock_type, sizeof(short), &socket->type);

    struct proto_ops *proto_ops = NULL;
    bpf_probe_read(&proto_ops, sizeof(proto_ops), &socket->ops);
    if (!proto_ops) {
        goto cleanup;
    }

    int family = 0;
    bpf_probe_read(&family, sizeof(family), &proto_ops->family);
    if (sock_type != SOCK_STREAM || !(family == AF_INET || family == AF_INET6)) {
        goto cleanup;
    }

    // Retrieve struct sock* pointer from struct socket*
    struct sock *sock = NULL;
    bpf_probe_read(&sock, sizeof(sock), &socket->sk);

    pid_fd_t pid_fd = {
        .pid = pid_tgid >> 32,
        .fd = (*sockfd),
    };

    // These entries are cleaned up by tcp_close
    bpf_map_update_elem(&pid_fd_by_sock, &sock, &pid_fd, BPF_ANY);
    bpf_map_update_elem(&sock_by_pid_fd, &pid_fd, &sock, BPF_ANY);
cleanup:
    bpf_map_delete_elem(&sockfd_lookup_args, &pid_tgid);
    return 0;
}

SEC("kprobe/do_sendfile")
int kprobe__do_sendfile(struct pt_regs* ctx) {
    u32 fd_out = (int)PT_REGS_PARM1(ctx);
    u64 pid_tgid = bpf_get_current_pid_tgid();
    pid_fd_t key = {
        .pid = pid_tgid >> 32,
        .fd = fd_out,
    };
    struct sock** sock = bpf_map_lookup_elem(&sock_by_pid_fd, &key);
    if (sock == NULL) {
        return 0;
    }

    // bring map value to eBPF stack to satisfy Kernel 4.4 verifier
    struct sock* skp = *sock;
    bpf_map_update_elem(&do_sendfile_args, &pid_tgid, &skp, BPF_ANY);
    return 0;
}

SEC("kretprobe/do_sendfile")
int kretprobe__do_sendfile(struct pt_regs* ctx) {
    u64 pid_tgid = bpf_get_current_pid_tgid();
    struct sock** sock = bpf_map_lookup_elem(&do_sendfile_args, &pid_tgid);
    if (sock == NULL) {
        return 0;
    }

    conn_tuple_t t = {};
    if (!read_conn_tuple(&t, *sock, pid_tgid, CONN_TYPE_TCP)) {
        goto cleanup;
    }

    size_t sent = (size_t)PT_REGS_RC(ctx);
    __u32 packets_in = 0;
    __u32 packets_out = 0;
    get_tcp_segment_counts(*sock, &packets_in, &packets_out);
    handle_message(&t, sent, 0, CONN_DIRECTION_UNKNOWN, packets_out, packets_in, PACKET_COUNT_ABSOLUTE);
cleanup:
    bpf_map_delete_elem(&do_sendfile_args, &pid_tgid);
    return 0;
}

//endregion

// This number will be interpreted by elf-loader to set the current running kernel version
__u32 _version SEC("version") = 0xFFFFFFFE; // NOLINT(bugprone-reserved-identifier)

char _license[] SEC("license") = "GPL"; // NOLINT(bugprone-reserved-identifier)
