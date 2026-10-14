/* SPDX-License-Identifier: MIT */
/* One-shot fourteen-core experiment; Linux permanently owns released CPUs. */
#include "direct-smp.h"
#include "adt.h"
#include "chickens.h"
#include "utils.h"
#include "j616s_screen.h"
#include "payload_sizes.h"
#include "loader-speed.h"
#include "fdt.h"
#include <string.h>

#ifdef M4_SMP_TEST
extern uint64_t m4_smp_test_read(const char *);
extern void m4_smp_test_write(const char *, uint64_t);
extern uint64_t m4_smp_test_read64(uint64_t);
extern void m4_smp_test_write32(uint64_t, uint32_t);
#define READ(r) m4_smp_test_read(#r)
#define WRITE(r,v) m4_smp_test_write(#r,v)
#define READ64(p) m4_smp_test_read64(p)
#define WRITE32(p,v) m4_smp_test_write32(p,v)
#define BARRIER() __asm__ volatile("":::"memory")
#define SYNC() BARRIER()
#else
/* Object aliases preserve utils.h's encoded Apple sysreg tuple parsing. */
#define READ mrs
#define WRITE msr
#define READ64(p) read64(p)
#define WRITE32(p,v) write32(p,v)
#define BARRIER() __asm__ volatile("dsb sy":::"memory")
#define SYNC() __asm__ volatile("isb":::"memory")
#endif

#define PRIMARY_HCR UINT64_C(0x30488000000)
extern char _vectors_start[], m4_direct_smp_reset_start[], m4_direct_smp_reset_end[];
extern char m4_direct_smp_release_start[], m4_direct_smp_release_end[];

struct m4_direct_smp_state m4_direct_smp_states[M4_SMP_MAX_PEERS] __attribute__((aligned(M4_SMP_PAGE)));
__asm__(".global m4_direct_smp_state\n.set m4_direct_smp_state,m4_direct_smp_states\n");
struct m4_direct_peer_record m4_direct_smp_peers[M4_SMP_MAX_PEERS] __attribute__((aligned(M4_SMP_PAGE)));
uint64_t m4_direct_smp_peer_count;
uint64_t m4_direct_smp_record_count;
static unsigned char secondary_stacks[M4_SMP_MAX_PEERS][M4_SMP_STACK] __attribute__((aligned(M4_SMP_PAGE)));
const uint64_t m4_direct_indexed_native_gate = 1;
volatile uint64_t m4_direct_smp_pending_peer;
int m4_direct_indexed_admitted(void)
{
#ifdef M4_SMP_TEST
    extern int m4_smp_test_admitted;
    return m4_smp_test_admitted;
#else
    return 1;
#endif
}

static struct m4_range retained[5];
static unsigned prepared, prepare_attempted, owned_attempted, primary_ready, start_set_ready;
static struct m4_range smp_arena;
static uint64_t pmgr_base;
static const struct {
    struct m4_direct_peer_plan plan;
    uint64_t impl_base;
    uint32_t global_mask, local_mask, cluster;
} backends[] = {
    {{5, 0x101, 0x10101}, UINT64_C(0x211150000), 0x20, 0x2, 1},
    {{6, 0x102, 0x10102}, UINT64_C(0x211250000), 0x40, 0x4, 1},
    {{7, 0x103, 0x10103}, UINT64_C(0x211350000), 0x80, 0x8, 1},
    {{8, 0x104, 0x10104}, UINT64_C(0x211450000), 0x100, 0x10, 1},
    {{10, 0x200, 0x10200}, UINT64_C(0x212050000), 0x400, 0x1, 2},
    {{11, 0x201, 0x10201}, UINT64_C(0x212150000), 0x800, 0x2, 2},
    {{12, 0x202, 0x10202}, UINT64_C(0x212250000), 0x1000, 0x4, 2},
    {{13, 0x203, 0x10203}, UINT64_C(0x212350000), 0x2000, 0x8, 2},
    {{14, 0x204, 0x10204}, UINT64_C(0x212450000), 0x4000, 0x10, 2},
    {{0, 0, 0}, UINT64_C(0x210050000), 0x1, 0x1, 0},
    {{1, 1, 1}, UINT64_C(0x210150000), 0x2, 0x2, 0},
    {{2, 2, 2}, UINT64_C(0x210250000), 0x4, 0x4, 0},
    {{3, 3, 3}, UINT64_C(0x210350000), 0x8, 0x8, 0},
};
_Static_assert(sizeof(backends)/sizeof(*backends) == M4_SMP_NATIVE_PEERS, "native peer count");
static int fail(struct m4_direct_smp_state *s, unsigned reason)
{
    s->error = reason;
    BARRIER(); s->phase = M4_SMP_FAILED; BARRIER();
    return -1;
}
static const void *exact(int node, const char *key, uint32_t bytes)
{
    uint32_t n; const void *p = adt_getprop(adt, node, key, &n);
    return p && n == bytes ? p : NULL;
}
static int word(int node, const char *key, uint32_t value)
{
    uint32_t v; const void *p = exact(node, key, 4);
    if (!p) return 0;
    memcpy(&v, p, 4); return v == value;
}
/* Exact current P endpoints; the sparse physical ID9 is absent. */
static int live_plan(void)
{
    if (!adt || adt_check_header(adt)) return -1;
    int trace[8] = {0}, node = adt_path_offset_trace(adt, "/arm-io/pmgr", trace);
    int arm = adt_path_offset(adt, "/arm-io"), primary = adt_path_offset(adt, "/cpus/cpu4");
    int chosen = adt_path_offset(adt, "/chosen");
    if (node < 0 || arm < 0 || primary < 0 || chosen < 0 || !word(chosen, "chip-id", T6040) ||
        !word(primary, "cpu-id", 4) || !word(primary, "reg", 0x100) ||
        !word(node, "misc-cores-offset", 0x88000) || !word(node, "AAPL,phandle", 0x109) ||
        !word(node, "acc-harvesting", 1)) return -1;
    const uint16_t clusters[] = {4, 0, 6, 4, 6, 10};
    const void *cluster_data = exact(node, "clusters", sizeof(clusters));
    if (!cluster_data || memcmp(cluster_data, clusters, sizeof(clusters))) return -1;
    uint64_t v[2];
    for (unsigned index = 0; index < M4_SMP_P_PEERS; index++) {
        int cpu = adt_path_offset(adt, (const char *const[]){"/cpus/cpu5", "/cpus/cpu6", "/cpus/cpu7", "/cpus/cpu8", "/cpus/cpu10", "/cpus/cpu11", "/cpus/cpu12", "/cpus/cpu13", "/cpus/cpu14"}[index]);
        if (cpu < 0 || !word(cpu, "cpu-id", (uint32_t)backends[index].plan.cpu_id) ||
            !word(cpu, "reg", (uint32_t)backends[index].plan.adt_reg) ||
            !word(cpu, "die-id", 0) || !word(cpu, "cluster-id", backends[index].cluster) ||
            !word(cpu, "l2-cache-size", 0x1000000) || !word(cpu, "l2-cache-id", 0)) return -1;
        const void *impl = exact(cpu, "cpu-impl-reg", 16), *state = exact(cpu, "state", 8);
        const void *type = exact(cpu, "cluster-type", 2), *compat = exact(cpu, "compatible", 21);
        const void *enable = exact(cpu, "function-enable_core", 12);
        const uint32_t token[] = {0x109, 0x436f7265, backends[index].global_mask};
        if (!impl || !state || memcmp(state, "waiting", 8) || !type || memcmp(type, "P", 2) ||
            !compat || memcmp(compat, "apple,everest\0ARM,v8", 21) ||
            !enable || memcmp(enable, token, sizeof(token))) return -1;
        memcpy(v, impl, sizeof(v));
        if (v[0] != backends[index].impl_base || v[1] != 0x9010) return -1;
    }
    for(unsigned n=0;n<M4_SMP_E_PEERS;n++) {
        int cpu=adt_path_offset(adt,(const char *const[]){"/cpus/cpu0","/cpus/cpu1","/cpus/cpu2","/cpus/cpu3"}[n]);
        const uint32_t token[]={0x109,0x436f7265,1U<<n};
        if(cpu<0 || !word(cpu,"cpu-id",n) || !word(cpu,"reg",n) ||
           !word(cpu,"die-id",0) || !word(cpu,"cluster-id",0) ||
           !word(cpu,"l2-cache-size",0x400000) || !word(cpu,"l2-cache-id",0))return -1;
        const void *impl=exact(cpu,"cpu-impl-reg",16), *state=exact(cpu,"state",8);
        const void *type=exact(cpu,"cluster-type",2), *compat=exact(cpu,"compatible",22);
        const void *enable=exact(cpu,"function-enable_core",12);
        if(!impl || !state || memcmp(state,"waiting",8) || !type || memcmp(type,"E",2) ||
           !compat || memcmp(compat,"apple,sawtooth\0ARM,v8",22) || !enable || memcmp(enable,token,sizeof(token)))return -1;
        memcpy(v,impl,sizeof(v));
        if(v[0]!=UINT64_C(0x210050000)+(uint64_t)n*0x100000 || v[1]!=0x9010)return -1;
    }
    uint32_t bytes; const unsigned char *regs = adt_getprop(adt, node, "reg", &bytes);
    if (!regs || bytes < 16 || bytes % 16) return -1;
    memcpy(v, regs, 16);
    if (v[0] != UINT64_C(0x302800000) || v[1] != 0x1e0000) return -1;
    const unsigned char *ranges = adt_getprop(adt, arm, "ranges", &bytes);
    if (!ranges || !bytes || bytes % 24) return -1;
    unsigned matches = 0;
    for (unsigned n = 0; n < bytes; n += 24) {
        uint64_t r[3]; memcpy(r, ranges + n, 24);
        if (!r[2] || r[0] > UINT64_MAX-r[2] || r[1] > UINT64_MAX-r[2]) return -1;
        if (v[0] >= r[0] && v[0]+v[1] <= r[0]+r[2]) {
            if (r[1]+v[0]-r[0] != UINT64_C(0x502800000)) return -1;
            matches++;
        }
    }
    uint64_t size;
    if (matches != 1 || adt_get_reg(adt, trace, "reg", 0, &pmgr_base, &size) ||
        pmgr_base != UINT64_C(0x502800000) || size != 0x1e0000) return -1;
    return 0;
}
static struct m4_range rounded(uint64_t first, uint64_t last)
{
    uint64_t base = first & ~(uint64_t)(M4_SMP_PAGE-1);
    return (struct m4_range){base, ((last+M4_SMP_PAGE-1)&~(uint64_t)(M4_SMP_PAGE-1))-base};
}
/* Register subset reused from accepted cold/extra captures. No dormant TCR,
 * MAIR, TTBR, stage2 or Apple memory-protection state is manufactured. */
static void capture(struct m4_direct_smp_state *s, uint64_t *out)
{
#define GET(id,r) do { s->active = 0x100+(id); BARRIER(); out[id] = READ(r); BARRIER(); } while (0)
    GET(S_SCTLR, SCTLR_EL2); GET(S_HCR, HCR_EL2);
    GET(S_GSCTLR, SCTLR_EL12); GET(S_MIDR, MIDR_EL1); GET(S_DAIF, DAIF); GET(S_SPSEL, SPSel);
    GET(S_TCR, TCR_EL2); GET(S_TTBR0, TTBR0_EL2); GET(S_MAIR, MAIR_EL2);
    GET(S_GTCR, TCR_EL12); GET(S_GTTBR0, TTBR0_EL12); GET(S_GTTBR1, TTBR1_EL12); GET(S_GMAIR, MAIR_EL12);
    GET(S_CNTHCTL, CNTHCTL_EL2); GET(S_CNTVOFF, CNTVOFF_EL2); GET(S_CPTR, CPTR_EL2);
    GET(S_CTR, CTR_EL0); GET(S_FRQ, CNTFRQ_EL0);
    GET(S_MMFR0, ID_AA64MMFR0_EL1); GET(S_PFR0, ID_AA64PFR0_EL1); GET(S_PFR1, ID_AA64PFR1_EL1);
    s->active = 0; BARRIER();
#undef GET
}
/* Independent architectural IDs fill the future heterogeneous-feature gap.
 * A fault leaves the completion bitmap and active register index intact. */
static void capture_extra(struct m4_direct_smp_state *s,uint64_t *out,uint64_t *done)
{
#define GET(id,r) do {s->active=0x200+(id);BARRIER();out[id]=READ(r);BARRIER();*done|=UINT64_C(1)<<(id);BARRIER();} while(0)
    GET(0,ID_AA64ISAR0_EL1);GET(1,ID_AA64ISAR1_EL1);GET(2,ID_AA64ISAR2_EL1);GET(3,s3_0_c0_c6_3);
    GET(4,ID_AA64MMFR1_EL1);GET(5,ID_AA64MMFR2_EL1);GET(6,s3_0_c0_c7_3);GET(7,s3_0_c0_c7_4);
    GET(8,s3_0_c0_c4_2);GET(9,s3_0_c0_c4_4);GET(10,s3_0_c0_c4_5);
    GET(11,ID_AA64DFR0_EL1);GET(12,ID_AA64DFR1_EL1);GET(13,CLIDR_EL1);GET(14,DCZID_EL0);GET(15,REVIDR_EL1);
    s->active=0;BARRIER();
#undef GET
}
static int cold_valid(const uint64_t *s, uint64_t midr)
{
    return s[S_SCTLR] == 0x30000180 && s[S_HCR] == PRIMARY_HCR &&
        s[S_GSCTLR] == 0x30d50980 && s[S_MIDR] == midr &&
        s[S_DAIF] == 0x3c0 && s[S_SPSEL] == 1 && s[S_FRQ] == 1000000000;
}

int m4_direct_smp_prepare_owned(const struct m4_memory *m, struct m4_range loader,
    struct m4_range arena, struct m4_range protected,
    const struct m4_direct_peer_plan *plans, unsigned count)
{
    if (owned_attempted) return -1;
    owned_attempted = 1;
    if (!m || !plans || !count || count > M4_SMP_MAX_PEERS || m->free_count > M4_MAX_FREE ||
        !m4_contains(m->usable, loader) || !m4_contains(loader, arena) ||
        arena.size != M4_ARENA_BYTES) return -1;
    struct m4_direct_peer_plan selected[M4_SMP_MAX_PEERS];
    memcpy(selected, plans, count*sizeof(*plans));
    /* First peer keeps the accepted pair identity; no other MPIDR is invented. */
    if (selected[0].cpu_id != 5 || selected[0].adt_reg != 0x101 ||
        selected[0].affinity != 0x10101) return -1;
    for (unsigned i=0; i<count; i++) {
        const struct m4_direct_peer_plan *p=selected+i;
        if (p->cpu_id > 14 || p->cpu_id == 9 || p->cpu_id == 4 || p->adt_reg > 0xffffff ||
            p->adt_reg == 0x100 || (!p->affinity && (p->cpu_id || p->adt_reg)) || p->affinity == 0x10100 ||
            (p->affinity & ~UINT64_C(0xff00ffffff))) return -1;
        for (unsigned j=0; j<i; j++)
            if (p->cpu_id == selected[j].cpu_id || p->adt_reg == selected[j].adt_reg ||
                p->affinity == selected[j].affinity) return -1;
    }
    retained[0] = rounded((uint64_t)_vectors_start, (uint64_t)_vectors_start+2048);
    retained[1] = rounded((uint64_t)m4_direct_smp_reset_start, (uint64_t)m4_direct_smp_reset_end);
    /* Both native reset and terminal release code must remain outside RAM. */
    struct m4_range release_code = rounded((uint64_t)m4_direct_smp_release_start, (uint64_t)m4_direct_smp_release_end);
    uint64_t code_end = retained[1].base+retained[1].size;
    if (release_code.base+release_code.size > code_end) code_end = release_code.base+release_code.size;
    if (release_code.base < retained[1].base) retained[1].base = release_code.base;
    retained[1].size = code_end-retained[1].base;
    retained[2] = (struct m4_range){(uint64_t)m4_direct_smp_states, sizeof(m4_direct_smp_states)};
    retained[3] = (struct m4_range){(uint64_t)secondary_stacks, sizeof(secondary_stacks)};
    retained[4] = rounded((uint64_t)m4_direct_smp_peers, (uint64_t)(m4_direct_smp_peers+M4_SMP_MAX_PEERS));
    for (unsigned i = 0; i < 5; i++) {
        if (!m4_contains(loader, retained[i]) || !m4_contains(m->usable, retained[i]) ||
            m4_overlaps(arena, retained[i]) || (i >= 2 && m4_overlaps(protected, retained[i])) ||
            retained[i].base+retained[i].size > m->top) return -1;
        for (unsigned j = 0; j < m->free_count; j++) if (m4_overlaps(m->free[j], retained[i])) return -1;
        for (unsigned j = 0; j < i; j++) if (m4_overlaps(retained[j], retained[i])) return -1;
    }

    for (unsigned i=0; i<M4_SMP_MAX_PEERS; i++) {
        const struct m4_direct_smp_state *s=m4_direct_smp_states+i;
        if (s->magic || s->phase || s->release || m4_direct_smp_peers[i].state_base) return -1;
    }
    for (unsigned i=0; i<count; i++) {
        struct m4_direct_smp_state *s=m4_direct_smp_states+i;
        const struct m4_direct_peer_plan *p=selected+i;
        memset(s,0,sizeof(*s));
        s->magic=M4_SMP_MAGIC; s->stack_base=(uint64_t)secondary_stacks[i];
        s->stack_top=s->stack_base+M4_SMP_STACK; s->affinity=p->affinity;
        s->kernel_base=(arena.base+0x20000+0x1fffff)&~UINT64_C(0x1fffff);
        s->kernel_size=M4_KERNEL_MEMORY_BYTES;
        if (!m4_contains(arena,(struct m4_range){s->kernel_base,s->kernel_size})) return fail(s,1);
        *(uint64_t *)secondary_stacks[i]=UINT64_C(0x6163616e61727931);
        m4_direct_smp_peers[i]=(struct m4_direct_peer_record){p->cpu_id,p->adt_reg,p->affinity,
            (uint64_t)s,sizeof(*s),s->stack_base,M4_SMP_STACK,(uint64_t)&s->release};
        /* Primary/cold/stable registers stay zero until actually observed. */
        s->phase=M4_SMP_PREPARED;
    }
    m4_direct_smp_peer_count=count; m4_direct_smp_record_count=count; prepared=1; smp_arena=arena; BARRIER();
    return 0;
}
/* R48: the peers' kernel_size is the booted image's memory (an SSD kernel differs from the embedded one); nothing has been launched yet */
int m4_direct_smp_kernel_memory(uint64_t size)
{
    if (!prepared || m4_direct_smp_record_count > M4_SMP_MAX_PEERS || m4_direct_smp_launched()) return -1;
    for (unsigned n=0; n<m4_direct_smp_record_count; n++) {
        struct m4_direct_smp_state *s=m4_direct_smp_states+n;
        if (!size || !m4_contains(smp_arena,(struct m4_range){s->kernel_base,size})) return -1;
        s->kernel_size=size;
    }
    return 0;
}
const struct m4_direct_peer_record *m4_direct_smp_peer_records(unsigned *count)
{
    if (!count || !prepared || !m4_direct_smp_peer_count ||
        m4_direct_smp_peer_count > M4_SMP_MAX_PEERS) return NULL;
    *count=(unsigned)m4_direct_smp_peer_count;
    return m4_direct_smp_peers;
}
static int peer_index(const struct m4_direct_smp_state *s)
{
    if (!prepared || !m4_direct_smp_record_count || m4_direct_smp_record_count>M4_SMP_MAX_PEERS) return -1;
    for (unsigned n=0; n<m4_direct_smp_record_count; n++)
        if (s == m4_direct_smp_states+n && m4_direct_smp_peers[n].state_base==(uint64_t)s &&
            m4_direct_smp_peers[n].affinity==s->affinity &&
            m4_direct_smp_peers[n].stack_base==s->stack_base &&
            m4_direct_smp_peers[n].release_addr==(uint64_t)&s->release &&
            m4_direct_smp_peers[n].state_bytes==sizeof(*s) &&
            m4_direct_smp_peers[n].stack_bytes==M4_SMP_STACK &&
            s->magic==M4_SMP_MAGIC && s->stack_top-s->stack_base==M4_SMP_STACK) return (int)n;
    return -1;
}
static int native_backend(unsigned index)
{
    if (index >= M4_SMP_NATIVE_PEERS || m4_direct_smp_peer_count != M4_SMP_NATIVE_PEERS) return 0;
    const struct m4_direct_peer_record *peer = m4_direct_smp_peers+index;
    const struct m4_direct_peer_plan *plan = &backends[index].plan;
    return peer->cpu_id == plan->cpu_id && peer->adt_reg == plan->adt_reg &&
        peer->affinity == plan->affinity;
}
int m4_direct_smp_launched(void)
{
    if (m4_direct_smp_record_count > M4_SMP_MAX_PEERS) return 1;
    for (unsigned index = 0; index < m4_direct_smp_record_count; index++) {
        const struct m4_direct_smp_state *s=m4_direct_smp_states+index;
        if (s->attempted || s->actual_mpidr || s->reset_el || s->park_progress ||
            s->phase==M4_SMP_RESET || s->phase==M4_SMP_INIT ||
            s->phase==M4_SMP_READY || s->phase==M4_SMP_RELEASED) return 1;
    }
    return 0;
}
int m4_direct_smp_diagnostics_safe(void)
{
    uint64_t previous[M4_SMP_MAX_PEERS]; unsigned changes[M4_SMP_MAX_PEERS]={0};
    if(!prepared || !primary_ready || m4_direct_smp_peer_count!=M4_SMP_NATIVE_PEERS ||
       m4_direct_smp_record_count!=M4_SMP_MAX_PEERS)return 0;
    for(unsigned n=0;n<M4_SMP_MAX_PEERS;n++)previous[n]=m4_direct_smp_states[n].park_progress;
    for(unsigned sample=0;sample<20;sample++) {
        unsigned safe=0;
        for(unsigned n=0;n<M4_SMP_MAX_PEERS;n++) {
            const struct m4_direct_smp_state *s=m4_direct_smp_states+n;
            if(peer_index(s)!=(int)n || s->release ||
               !native_backend(n))return 0;
            if(!s->attempted && s->phase==M4_SMP_PREPARED && !s->error &&
               !s->actual_mpidr && !s->reset_el && !s->ready_counter && !s->active &&
               !s->park_progress && !s->park_daif && !s->park_mpidr && !s->park_el && !s->park_pc) {
                safe++;continue;
            }
            if(s->phase==M4_SMP_READY && s->attempted==1 && s->reset_el==8 && !s->active && !s->error &&
               s->actual_mpidr==s->affinity && s->ready_counter &&
               s->sctlr_after==0x30000180 && s->hcr_after==PRIMARY_HCR &&
               s->primary_ctr==s->secondary_ctr) {safe++;continue;}
            if(s->phase!=M4_SMP_FAILED || s->attempted!=1 || s->park_daif!=0x3c0 || s->park_el!=8 ||
               s->park_mpidr!=s->affinity || s->actual_mpidr!=s->affinity || s->reset_el!=8 ||
               s->park_pc!=(uint64_t)m4_direct_smp_failed_loop)return 0;
            uint64_t progress=s->park_progress;
            if(progress!=previous[n]) {previous[n]=progress;changes[n]++;}
            if(changes[n]>=2)safe++;
        }
        if(safe==M4_SMP_MAX_PEERS)return 1;
        udelay(1000);BARRIER();
    }
    return 0;
}
int m4_direct_smp_prepare(const struct m4_memory *m, struct m4_range loader,
                         struct m4_range arena, struct m4_range protected)
{
    if (prepare_attempted) return -1;
    prepare_attempted = 1;
    if (!m4_direct_indexed_admitted() || !m || READ(CurrentEL) != 8 ||
        READ(SCTLR_EL2) != 0x30000180 || READ(HCR_EL2) != PRIMARY_HCR ||
        READ(SCTLR_EL12) != 0x30d50980 || READ(MIDR_EL1) != 0x611f0551 ||
        (READ(MPIDR_EL1)&UINT64_C(0xff00ffffff)) != 0x10100 ||
        READ(DAIF) != 0x3c0 || READ(SPSel) != 1 || live_plan()) return -1;
    struct m4_direct_peer_plan selected[M4_SMP_MAX_PEERS];
    for (unsigned n=0; n<M4_SMP_NATIVE_PEERS; n++) selected[n]=backends[n].plan;
    if(m4_direct_smp_prepare_owned(m,loader,arena,protected,selected,M4_SMP_MAX_PEERS))return -1;
    struct m4_direct_smp_state *s=m4_direct_smp_states;
    init_cpu();
    if (!cpu_features->fast_ipi || !cpu_features->actlr_el2 ||
        cpu_features->apple_sysregs_unlocked || !cpu_features->sme_enabled) return fail(s,2);
    for (unsigned index = 0; index < M4_SMP_MAX_PEERS; index++) {
        s = m4_direct_smp_states+index;
        capture(s,s->primary);
        if (!cold_valid(s->primary, 0x611f0551)) return fail(s,3);
        s->frequency=s->primary[S_FRQ]; s->primary_ctr=s->primary[S_CTR];
        capture_extra(s,s->extra_primary,&s->extra_primary_complete);
    }
    primary_ready=1; BARRIER();
    return 0;
}
/* Pages the peers read or write with their MMU off: kept Device in the
 * primary's cached windows. Valid before prepare; fixed static storage. */
unsigned m4_direct_smp_owned_pages(struct m4_range *out, unsigned capacity)
{
    if (!out || capacity < 3) return 0;
    out[0] = (struct m4_range){(uint64_t)m4_direct_smp_states, sizeof(m4_direct_smp_states)};
    out[1] = (struct m4_range){(uint64_t)secondary_stacks, sizeof(secondary_stacks)};
    out[2] = rounded((uint64_t)m4_direct_smp_peers, (uint64_t)(m4_direct_smp_peers+M4_SMP_MAX_PEERS));
    return 3;
}
const struct m4_range *m4_direct_smp_regions(unsigned *count, uint64_t *release)
{
    if (!count || !release || !prepared || m4_direct_smp_state.phase != M4_SMP_PREPARED) return NULL;
    *count = 5; *release = (uint64_t)&m4_direct_smp_state.release;
    return retained;
}
static void park(void) __attribute__((noreturn));
static void park(void) { m4_direct_smp_failed_park(); }
void m4_direct_smp_secondary(struct m4_direct_smp_state *s)
{
    int index=peer_index(s);
    if(!m4_direct_indexed_admitted() || index<0)park();
    if(!native_backend((unsigned)index))park();
    if (READ(CurrentEL) != 8 || s->phase != M4_SMP_RESET ||
        (READ(MPIDR_EL1)&UINT64_C(0xff00ffffff)) != s->affinity) park();
    s->actual_mpidr = READ(MPIDR_EL1)&UINT64_C(0xff00ffffff);
    s->reset_el = READ(CurrentEL);
    s->sctlr_before = READ(SCTLR_EL2); s->hcr_before = READ(HCR_EL2);
    uint64_t mmfr = READ(ID_AA64MMFR0_EL1), pfr0 = READ(ID_AA64PFR0_EL1), pfr1 = READ(ID_AA64PFR1_EL1);
    uint64_t csv2 = (pfr0>>56)&15, frac = (pfr1>>32)&15;
    uint64_t midr = (unsigned)index<M4_SMP_P_PEERS ? UINT64_C(0x611f0551) : UINT64_C(0x611f0541);
    if (s->sctlr_before != 0x30d50980 || s->hcr_before != PRIMARY_HCR ||
        READ(MIDR_EL1) != midr || READ(DAIF) != 0x3c0 || READ(SPSel) != 1 ||
        ((mmfr>>44)&15) != 1 || !((csv2 == 1 && frac == 2) || ((csv2 == 2 || csv2 == 3) && frac == 0)) ||
        mmfr != s->primary[S_MMFR0] || pfr0 != s->primary[S_PFR0] || pfr1 != s->primary[S_PFR1]) { fail(s,20); park(); }
    capture(s,s->reset);
    capture(s,s->stable);
    if(memcmp(s->reset,s->stable,sizeof(s->reset))) {fail(s,95);park();}
    capture_extra(s,s->extra_reset,&s->extra_reset_complete);
    capture_extra(s,s->extra_stable,&s->extra_stable_complete);
    if(s->extra_stable_complete!=65535 ||
       memcmp(s->extra_reset,s->extra_stable,sizeof(s->extra_reset))) {fail(s,96);park();}
    /* The native E0–3 reference permits only the observed raw SCTLR/MIDR
     * and dormant TCR/MAIR differences from this independently captured P. */
    if((unsigned)index>=M4_SMP_P_PEERS) {
        for(unsigned reg=0;reg<S_REGS;reg++) {
            uint64_t expected=s->primary[reg];
            if(reg==S_SCTLR)expected=0x30d50980;
            else if(reg==S_MIDR)expected=0x611f0541;
            else if(reg==S_TCR)expected=UINT64_C(0x340008000);
            else if(reg==S_MAIR)expected=0;
            if(s->reset[reg]!=expected) {fail(s,70+reg);park();}
        }
    }
    if(s->extra_primary_complete!=65535 || s->extra_reset_complete!=65535 ||
       memcmp(s->extra_primary,s->extra_reset,sizeof(s->extra_reset))) {fail(s,92);park();}
    /* Capture this peer before the accepted reset profile changes any state. */
    BARRIER(); WRITE(SCTLR_EL2, 0x30000180); SYNC();
    if (READ(SCTLR_EL2) != 0x30000180 || READ(HCR_EL2) != PRIMARY_HCR) { fail(s,21); park(); }
    init_cpu();
    if (!cpu_features->fast_ipi || !cpu_features->actlr_el2 ||
        cpu_features->apple_sysregs_unlocked || !cpu_features->sme_enabled) {fail(s,93);park();}
    /* Accepted exception_initialize's source masking, without its shared
     * vectors/DAIF relaxation or cooperative HCR routing. Linux owns HCR. */
    WRITE(CNTP_CTL_EL0, 7); WRITE(CNTV_CTL_EL0, 7);
    WRITE(CNTP_CTL_EL02, 7); WRITE(CNTV_CTL_EL02, 7);
    uint64_t pmcr = READ(SYS_IMP_APL_PMCR0);
    WRITE(SYS_IMP_APL_PMCR0, pmcr & ~(uint64_t)(PMCR0_IACT | PMCR0_IMODE_MASK));
    WRITE(SYS_IMP_APL_IPI_SR_EL1, IPI_SR_PENDING);
    BARRIER();
    capture(s,s->secondary); capture(s,s->stable);
    if (!cold_valid(s->secondary, midr) || memcmp(s->secondary, s->stable, sizeof(s->secondary))) { fail(s,22); park(); }
    for (unsigned i = 0; i < S_REGS; i++) {
        if (i == S_TCR || i == S_MAIR || i == S_MIDR) continue;
        if (s->primary[i] != s->secondary[i]) { fail(s,30+i); park(); }
    }
    /* Admit only directly observed equal or accepted dormant translation state. */
    if (!((s->primary[S_TCR] == s->secondary[S_TCR] && s->primary[S_MAIR] == s->secondary[S_MAIR]) ||
          (s->primary[S_TCR] == 0 && s->primary[S_MAIR] == UINT64_C(0x44401ff05) &&
           s->secondary[S_TCR] == UINT64_C(0x340008000) && s->secondary[S_MAIR] == 0 &&
           s->primary[S_TTBR0] == 0 && s->secondary[S_TTBR0] == 0))) { fail(s,53); park(); }
    s->extra_stable_complete=0;BARRIER();
    capture_extra(s,s->extra_stable,&s->extra_stable_complete);
    if(s->extra_stable_complete!=65535 ||
       memcmp(s->extra_primary,s->extra_stable,sizeof(s->extra_stable))) {fail(s,94);park();}
    if (*(uint64_t *)s->stack_base != UINT64_C(0x6163616e61727931)) { fail(s,54); park(); }
    s->sctlr_after = s->secondary[S_SCTLR]; s->hcr_after = s->secondary[S_HCR];
    s->secondary_ctr = s->secondary[S_CTR];
    BARRIER(); s->phase = M4_SMP_INIT; BARRIER();
    m4_direct_smp_linux_wait(s);
}
#ifdef M4_SMP_SEQUENTIAL
static int m4_direct_smp_start_peer(unsigned index)
{
    if (!m4_direct_indexed_admitted() || !primary_ready || !start_set_ready || !native_backend(index)) return -1;
    struct m4_direct_smp_state *s = m4_direct_smp_states+index;
    if (peer_index(s)<0 || !prepared || s->phase != M4_SMP_PREPARED || s->attempted ||
        READ(CurrentEL) != 8 || (READ(MPIDR_EL1)&UINT64_C(0xff00ffffff)) != 0x10100 ||
        READ(SCTLR_EL2) != 0x30000180 || READ(HCR_EL2) != PRIMARY_HCR || live_plan()) return -1;
    s->active = 1;
    if ((READ64(backends[index].impl_base)&UINT64_C(0x0000fffffffff000)) != (uint64_t)_vectors_start) return fail(s,10);
    s->attempted = 1; m4_direct_smp_pending_peer = index+1; BARRIER();
    /* Source-backed tuple masks; CPU10 global0x400 must not alias CPU8.
     * RVBAR stays read-only; actual reset affinity/cold state gate release. */
    WRITE32(pmgr_base+0x88004, backends[index].global_mask);
    WRITE32(pmgr_base+0x88008+4*backends[index].cluster, backends[index].local_mask);
    BARRIER();
    for (unsigned i = 0; i < 300; i++) {
        BARRIER();
        if (s->phase == M4_SMP_READY) {
            if (s->attempted!=1 || s->reset_el!=8 || s->active || s->error || s->release || s->actual_mpidr != s->affinity || !s->ready_counter ||
                s->sctlr_after != 0x30000180 || s->hcr_after != PRIMARY_HCR ||
                s->primary_ctr != s->secondary_ctr) return -1;
            m4_direct_smp_pending_peer = 0; BARRIER();
            return 0;
        }
        if (s->phase == M4_SMP_FAILED) return -1;
        udelay(1000);
    }
    /* Do not overwrite a late secondary's phase or recycle its allocation. */
    return -1;
}
#else
/* Pipelined start. As before, exactly one PMGR start request is outstanding:
 * the next core is launched only after the previous one has identified itself
 * in its own record (left PREPARED). Cores then initialise concurrently and
 * each must reach READY within the former 300 ms budget from its own launch.
 * The live ADT plan is checked once for the whole set in m4_direct_smp_start. */
#define M4_SMP_POLL_US 10
#define M4_SMP_POLL_LIMIT (300000 / M4_SMP_POLL_US)
static int m4_direct_smp_launch(unsigned index)
{
    if (!m4_direct_indexed_admitted() || !primary_ready || !start_set_ready || !native_backend(index)) return -1;
    struct m4_direct_smp_state *s = m4_direct_smp_states+index;
    if (peer_index(s)<0 || !prepared || s->phase != M4_SMP_PREPARED || s->attempted ||
        m4_direct_smp_pending_peer || READ(CurrentEL) != 8 ||
        (READ(MPIDR_EL1)&UINT64_C(0xff00ffffff)) != 0x10100 ||
        READ(SCTLR_EL2) != 0x30000180 || READ(HCR_EL2) != PRIMARY_HCR) return -1;
    s->active = 1;
    if ((READ64(backends[index].impl_base)&UINT64_C(0x0000fffffffff000)) != (uint64_t)_vectors_start) return fail(s,10);
    s->attempted = 1; m4_direct_smp_pending_peer = index+1; BARRIER();
    WRITE32(pmgr_base+0x88004, backends[index].global_mask);
    WRITE32(pmgr_base+0x88008+4*backends[index].cluster, backends[index].local_mask);
    BARRIER();
    return 0;
}
static int m4_direct_smp_ready(const struct m4_direct_smp_state *s)
{
    return s->attempted==1 && s->reset_el==8 && !s->active && !s->error && !s->release &&
        s->actual_mpidr == s->affinity && s->ready_counter &&
        s->sctlr_after == 0x30000180 && s->hcr_after == PRIMARY_HCR &&
        s->primary_ctr == s->secondary_ctr;
}
static int m4_direct_smp_start_pipelined(unsigned count)
{
    unsigned launched[M4_SMP_MAX_PEERS], polls = 0;
    m4_speed.smp_mode = 1;
    m4_speed.smp_first_launch = READ(CNTPCT_EL0);
    for (unsigned n = 0; n < count; n++) {
        const struct m4_direct_smp_state *s = m4_direct_smp_states+n;
        /* As the sequential start did, no further launch after any failure. */
        for (unsigned k = 0; k < n; k++)
            if (m4_direct_smp_states[k].phase == M4_SMP_FAILED) return -1;
        if (m4_direct_smp_launch(n)) return -1;
        launched[n] = polls;
        for (;;) {
            BARRIER();
            uint64_t phase = s->phase;
            if (phase == M4_SMP_FAILED) return -1;
            if (phase != M4_SMP_PREPARED) break;
            if (polls - launched[n] >= M4_SMP_POLL_LIMIT) return -1;
            udelay(M4_SMP_POLL_US); polls++;
        }
        m4_direct_smp_pending_peer = 0; BARRIER();
    }
    m4_speed.smp_last_running = READ(CNTPCT_EL0);
    for (unsigned n = 0; n < count; n++) {
        const struct m4_direct_smp_state *s = m4_direct_smp_states+n;
        for (;;) {
            BARRIER();
            uint64_t phase = s->phase;
            if (phase == M4_SMP_READY) {
                if (!m4_direct_smp_ready(s)) return -1;
                break;
            }
            if (phase == M4_SMP_FAILED) return -1;
            /* Do not overwrite a late secondary's phase or recycle its allocation. */
            if (polls - launched[n] >= M4_SMP_POLL_LIMIT) return -1;
            udelay(M4_SMP_POLL_US); polls++;
        }
    }
    m4_speed.smp_all_ready = READ(CNTPCT_EL0);
    return 0;
}
#endif

int m4_direct_smp_start(void)
{
    if (!m4_direct_indexed_admitted() || !prepared || !primary_ready ||
        !m4_direct_smp_peer_count || m4_direct_smp_peer_count>M4_SMP_MAX_PEERS) return -1;
    if (start_set_ready || live_plan()) return -1;
    /* Check every endpoint and locked vector before either PMGR launch. */
    for (unsigned n=0;n<m4_direct_smp_peer_count;n++) {
        if (!native_backend(n) || peer_index(m4_direct_smp_states+n) != (int)n ||
            m4_direct_smp_states[n].phase != M4_SMP_PREPARED ||
            m4_direct_smp_states[n].attempted || m4_direct_smp_states[n].release ||
            m4_direct_smp_states[n].error ||
            *(uint64_t *)m4_direct_smp_states[n].stack_base != UINT64_C(0x6163616e61727931)) return -1;
        if ((READ64(backends[n].impl_base)&UINT64_C(0x0000fffffffff000)) !=
            (uint64_t)_vectors_start) return fail(m4_direct_smp_states+n,10);
    }
    start_set_ready = 1; BARRIER();
#ifdef M4_SMP_SEQUENTIAL
    for (unsigned n=0;n<m4_direct_smp_peer_count;n++) if (m4_direct_smp_start_peer(n)) return -1;
#else
    if (m4_direct_smp_start_pipelined((unsigned)m4_direct_smp_peer_count)) return -1;
#endif
    return m4_direct_smp_diagnostics_safe()?0:-1;
}
void m4_direct_smp_report(void)
{
    if (m4_direct_smp_launched() && !m4_direct_smp_diagnostics_safe()) return;
    for (unsigned n=0;n<m4_direct_smp_peer_count && n<M4_SMP_MAX_PEERS;n++) {
    const struct m4_direct_smp_state *s = m4_direct_smp_states+n;
    /* one line per peer; the register detail only for a peer that faulted or is in an error state */
    j616s_screen_printf("peer=%u cpu=%lu phase=%lu error=%lu active=%lu mpidr=0x%lx rel=0x%lx\n",n,m4_direct_smp_peers[n].cpu_id,
                        s->phase, s->error, s->active, s->actual_mpidr, s->release);
    if (s->error || s->esr || s->elr || s->far)
        j616s_screen_printf(" fault ESR=0x%lx ELR=0x%lx FAR=0x%lx SP=0x%lx SCTLR=0x%lx/0x%lx\n", s->esr, s->elr, s->far, s->fault_sp,
                            s->sctlr_before, s->sctlr_after);
    }
    j616s_screen_flush();
}
