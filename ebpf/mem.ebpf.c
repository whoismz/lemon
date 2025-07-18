#ifdef CORE
    #include "../vmlinux.h"
    #include <bpf/bpf_core_read.h>

    #ifndef ETH_P_IP
        #define ETH_P_IP 0x0800
    #endif

    #ifndef IPPROTO_UDP
        #define IPPROTO_UDP 17
    #endif

#elif NOCORE
    #include <linux/bpf.h>
    #include <asm/ptrace.h>
    #include <linux/if_ether.h>
    #include <linux/in.h>
    #include <linux/ip.h>
    #include <linux/udp.h>

#elif NOCOREUNI
    #include "../nocore_universal.h"
    #include <bpf/bpf_tracing.h>
    
#endif

#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>
#include <bpf/bpf_endian.h>

#include "../lemon.h"

/* Mapping used to pass the memory content to userspace */
struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __type(key, int);
    __type(value, struct read_mem_result);
    __uint(max_entries, 1);
    __uint(map_flags, BPF_F_MMAPABLE | BPF_F_NUMA_NODE);
} read_mem_array_map SEC(".maps");

/* VA bits for ARM64 
 *
 * Try to get the va bits from the kernel config.
 * Otherwise we try to compute the actual va bits (runtime) in userspace and inject it here on eBPF
 * program open.
 */
#ifdef __TARGET_ARCH_arm64
    extern unsigned long CONFIG_ARM64_VA_BITS __kconfig __weak;
    unsigned long runtime_va_bits SEC(".data");
#endif

/*
 * read_memory() - Read kernel memory and save the content in the eBPF map
 *
 * Attempts to read a specified chunk of kernel memory starting from a given address,
 * validating the request against architecture-specific constraints and dump size limits.
 * The memory contents are copied into a BPF map for retrieval from userspace.
 * Returns 0 on success or parameter validation failure, and -1 if the BPF map is unavailable.
 * Return also a specific error code in the map.
 */
static int inline read_memory(__u64 address, const __u64 dump_size) {
    /* Get the map in which save the memory content to pass to userspace */
    int key = 0;
    struct read_mem_result *read_mem_result = bpf_map_lookup_elem(&read_mem_array_map, &key);
    if (!read_mem_result) {
        return -1; // We cannot catch this error...
    }

    /* Validate dump size */
    if(dump_size > HUGE_PAGE_SIZE) {
        read_mem_result->ret_code = -EINVAL;
        return 0;
    }

    /* ARM64 phys to virt offset depends also on virtual addresses number of bits */
    #ifdef __TARGET_ARCH_arm64
        if (CONFIG_ARM64_VA_BITS != 0) {
            address |= 0xffffffffffffffff << CONFIG_ARM64_VA_BITS;
        } else {
            address |= 0xffffffffffffffff << runtime_va_bits;
        }
    #endif

    /* Ensure parameters are sanitized (some checks are needed to bypass eBPF type checking) */
    #ifdef __TARGET_ARCH_x86
        if (address < 0 || address < 0xff00000000000000){
    #elif __TARGET_ARCH_arm64
        if (address < 0 || address < 0xfff0000000000000){
    #else
        if(true){
    #endif
    
        read_mem_result->ret_code = -EINVAL;
        return 0;
    }

    if (dump_size < 0 || dump_size > HUGE_PAGE_SIZE) {
        read_mem_result->ret_code = -EINVAL;
        return 0;
    }

    /* Read the kernel memory */
    #ifdef CORE
        read_mem_result->ret_code = bpf_core_read((void *)(&read_mem_result->buf), (__u32)dump_size, (void *)address);
    #else
        read_mem_result->ret_code = bpf_probe_read_kernel((void *)(&read_mem_result->buf), (__u32)dump_size, (void *)address);
    #endif

    return 0;
}

/*
 * read_kernel_memory_uprobe() - Read kernel memory using a Uprobe trigger 
 *
 * Uprobe handler for extracting kernel memory from userspace-triggered instrumentation.
 * Retrieves the target address and dump size from the probed function’s arguments,
 */
SEC("uprobe//proc/self/exe:read_kernel_memory")
int read_kernel_memory_uprobe(struct pt_regs *ctx)
{
    /* Extract the first two arguments of the function */
    #ifdef CORE
        __u64 address = (__u64)(PT_REGS_PARM1_CORE(ctx));
        __u64 dump_size = (__u64)(PT_REGS_PARM2_CORE(ctx));
    #else
        __u64 address = (__u64)(PT_REGS_PARM1(ctx));
        __u64 dump_size = (__u64)(PT_REGS_PARM2(ctx));
    #endif

    /* Read memory! */
    return read_memory(address, dump_size);
}

#define TRIGGER_PACKET_PORT 9999
#define TRIGGER_PACKET_ADDR 0x7f000001 /* 127.0.0.1 */

/*
 * read_kernel_memory_xdp() - XDP program to trigger a kernel memory read
 * @ctx: Pointer to the XDP context containing packet metadata
 *
 * Parses a UDP packet containing address and size parameters used to
 * perform a kernel memory read. Expects UDP packets to 127.0.0.1:9999.
 */
SEC("xdp")
int read_kernel_memory_xdp(struct xdp_md* ctx) {
    void* data = (void*)(long)ctx->data;
    void* data_end = (void*)(long)ctx->data_end;

    /* Validate Ethernet header */
    struct ethhdr *eth = data;
    if ((void*)(eth + 1) > data_end) {
        return XDP_DROP;
    }

    /* Check if this is an IP packet */
    if (eth->h_proto != bpf_htons(ETH_P_IP)) {
        return XDP_PASS;
    }

    /* Validate and parse IP header */
    struct iphdr *ip = (struct iphdr*)(eth + 1);
    if ((void*)(ip + 1) > data_end) {
        return XDP_DROP;
    }

    /* Check if this is a UDP packet */
    if (ip->protocol != IPPROTO_UDP) {
        return XDP_PASS;
    }

    /* Check if source/dest is loopback */
    if (ip->saddr != bpf_htonl(TRIGGER_PACKET_ADDR) ||  ip->daddr != bpf_htonl(TRIGGER_PACKET_ADDR)) {
        return XDP_PASS;
    }

    /* Validate IP header length */
    if (ip->ihl < 5) {
        return XDP_DROP;
    }

    /* Validate UDP header */
    struct udphdr *udp = (struct udphdr*)((char*)ip + (ip->ihl * 4));
    if ((void*)(udp + 1) > data_end) {
        return XDP_DROP;
    }

    /* Check destination port */
    if (udp->dest != bpf_htons(TRIGGER_PACKET_PORT)) {
        return XDP_PASS;
    }

    /* Validate payload */
    struct read_mem_args *args = (struct read_mem_args*)(udp + 1);
    if ((void*)(args + 1) > data_end) {
        return XDP_DROP;
    }

    __u64 address = args->addr;
    __u64 dump_size = args->size;

    /* Read memory! */
    if (read_memory(address, dump_size)) {
        return XDP_DROP;
    }

    return XDP_PASS;
}

char _license[] SEC("license") = "GPL";
