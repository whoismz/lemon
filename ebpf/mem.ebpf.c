#ifdef CORE
    #include "../vmlinux.h"
    #include <bpf/bpf_core_read.h>
#else
    #include <linux/bpf.h>
    #include <linux/if_ether.h>
    #include <asm/ptrace.h>
#endif

#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>

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

/*
 * read_kernel_memory_xdp() - XDP program to trigger a kernel memory read
 * @ctx: Pointer to the XDP context containing packet metadata
 *
 * Parses a synthetic packet containing address and size parameters used to 
 * perform a kernel memory read.
 */
SEC("xdp")
 int read_kernel_memory_xdp(struct xdp_md* ctx) {
    int ret;
    void* data = (void*)(long)ctx->data;
    void* data_end = (void*)(long)ctx->data_end;
    
    /* Validate Ethernet header */
    struct ethhdr *eth = data;
    if ((void*)(eth + 1) > data_end) {
        return XDP_DROP;
    }
    
    /* Skip Ethernet header and validate payload */
    struct read_mem_args *args = (struct read_mem_args*)(eth + 1);
    if ((void*)(args + 1) > data_end) {
        return XDP_DROP;
    }

    __u64 address = args->addr;
    __u64 dump_size =  args->size;

    /* Read memory! */
    ret = read_memory(address, dump_size);
    if(ret) return XDP_DROP;

    return XDP_PASS;
}

char _license[] SEC("license") = "GPL";
