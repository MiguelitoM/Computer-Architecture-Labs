#include "tlb.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "clock.h"
#include "constants.h"
#include "log.h"
#include "memory.h"
#include "page_table.h"

typedef struct {
    bool valid;
    bool dirty;
    uint64_t last_access;
    va_t virtual_page_number;
    pa_dram_t physical_page_number;
} tlb_entry_t;

tlb_entry_t tlb_l1[TLB_L1_SIZE];
tlb_entry_t tlb_l2[TLB_L2_SIZE];

uint64_t tlb_l1_hits = 0;
uint64_t tlb_l1_misses = 0;
uint64_t tlb_l1_invalidations = 0;

uint64_t tlb_l2_hits = 0;
uint64_t tlb_l2_misses = 0;
uint64_t tlb_l2_invalidations = 0;

uint64_t get_total_tlb_l1_hits() { return tlb_l1_hits; }
uint64_t get_total_tlb_l1_misses() { return tlb_l1_misses; }
uint64_t get_total_tlb_l1_invalidations() { return tlb_l1_invalidations; }

uint64_t get_total_tlb_l2_hits() { return tlb_l2_hits; }
uint64_t get_total_tlb_l2_misses() { return tlb_l2_misses; }
uint64_t get_total_tlb_l2_invalidations() { return tlb_l2_invalidations; }

void tlb_init() {
    memset(tlb_l1, 0, sizeof(tlb_l1));
    memset(tlb_l2, 0, sizeof(tlb_l2));
    tlb_l1_hits = 0;
    tlb_l1_misses = 0;
    tlb_l1_invalidations = 0;
    tlb_l2_hits = 0;
    tlb_l2_misses = 0;
    tlb_l2_invalidations = 0;
}

void tlb_invalidate(va_t virtual_page_number) {
    // Invalidar entrada na L1
    for (int i = 0; i < TLB_L1_SIZE; i++) {
        if (tlb_l1[i].valid && tlb_l1[i].virtual_page_number == virtual_page_number) {
            if (tlb_l1[i].dirty) {
                write_back_tlb_entry(tlb_l1[i].physical_page_number << PAGE_SIZE_BITS);
            }
            tlb_l1[i].valid = false;
            tlb_l1_invalidations++;
            log_dbg("TLB L1: Invalidated entry at index %d for VPN %" PRIx64, i, virtual_page_number);
        }
    }
    increment_time(TLB_L1_LATENCY_NS);

    // Invalidar entrada na L2
    for (int i = 0; i < TLB_L2_SIZE; i++) {
        if (tlb_l2[i].valid && tlb_l2[i].virtual_page_number == virtual_page_number) {
            if (tlb_l2[i].dirty) {
                write_back_tlb_entry(tlb_l2[i].physical_page_number << PAGE_SIZE_BITS);
            }
            tlb_l2[i].valid = false;
            tlb_l2_invalidations++;
            log_dbg("TLB L2: Invalidated entry at index %d for VPN %" PRIx64, i, virtual_page_number);
        }
    }
    increment_time(TLB_L2_LATENCY_NS);
}

static void insert_into_tlb(tlb_entry_t *tlb, int tlb_size, va_t virtual_page_number,
                            pa_dram_t physical_page_number, bool is_dirty, bool is_l2,
                            uint64_t access_time) {
    int lru_index = -1;
    uint64_t oldest_time = UINT64_MAX;

    // Confirmar se a entrada já existe
    if (is_l2) {
        for (int i = 0; i < tlb_size; i++) {
            if (tlb[i].valid && tlb[i].virtual_page_number == virtual_page_number) {
                tlb[i].dirty |= is_dirty;
                tlb[i].last_access = access_time;
                return;
            }
        }
    }

    // Procurar entrada inválida ou LRU
    for (int i = 0; i < tlb_size; i++) {
        if (!tlb[i].valid) {
            lru_index = i;
            break;
        }
        if (tlb[i].last_access < oldest_time) {
            oldest_time = tlb[i].last_access;
            lru_index = i;
        }
    }

    // Guardar a entrada expulsa
    bool evict_valid   = tlb[lru_index].valid;
    bool evict_dirty   = tlb[lru_index].dirty;
    va_t evict_vpn     = tlb[lru_index].virtual_page_number;
    pa_dram_t evict_ppn = tlb[lru_index].physical_page_number;
    uint64_t evict_time = tlb[lru_index].last_access;

    // Se for L2 → write-back em caso de dirty
    if (is_l2 && evict_valid && evict_dirty) {
        write_back_tlb_entry(evict_ppn << PAGE_SIZE_BITS);
    }

    // Inserir nova entrada no nível atual
    tlb[lru_index].valid = true;
    tlb[lru_index].dirty = is_dirty;
    tlb[lru_index].last_access = access_time;
    tlb[lru_index].virtual_page_number = virtual_page_number;
    tlb[lru_index].physical_page_number = physical_page_number;

    // Se for L1 e expulsámos algo válido → empurrar para L2
    if (!is_l2 && evict_valid) {
        insert_into_tlb(tlb_l2, TLB_L2_SIZE, evict_vpn, evict_ppn, evict_dirty, true, evict_time);
    }
}

pa_dram_t tlb_translate(va_t virtual_address, op_t op) {
    
    va_t virtual_page_number = virtual_address >> PAGE_SIZE_BITS;

    // ---------------- L1 Lookup ----------------
    increment_time(TLB_L1_LATENCY_NS);
    for (int i = 0; i < TLB_L1_SIZE; i++) {
        if (tlb_l1[i].valid && tlb_l1[i].virtual_page_number == virtual_page_number) {
            tlb_l1_hits++;

            tlb_l1[i].last_access = get_time();
            if (op == OP_WRITE) {
                tlb_l1[i].dirty = true;
            }

            return (tlb_l1[i].physical_page_number << PAGE_SIZE_BITS) |
                   (virtual_address & PAGE_OFFSET_MASK);
        }
    }

    // L1 miss
    tlb_l1_misses++;

    // ---------------- L2 Lookup ----------------
    increment_time(TLB_L2_LATENCY_NS);
    for (int i = 0; i < TLB_L2_SIZE; i++) {
        if (tlb_l2[i].valid && tlb_l2[i].virtual_page_number == virtual_page_number) {
            tlb_l2_hits++;

            tlb_l2[i].last_access = get_time();
            if (op == OP_WRITE) {
                tlb_l2[i].dirty = true;
            }

            // Promoção da L2 para L1
            insert_into_tlb(tlb_l1, TLB_L1_SIZE, tlb_l2[i].virtual_page_number,
                            tlb_l2[i].physical_page_number, tlb_l2[i].dirty, false, get_time());

            return (tlb_l2[i].physical_page_number << PAGE_SIZE_BITS) |
                   (virtual_address & PAGE_OFFSET_MASK);
        }
    }

    // L2 miss
    tlb_l2_misses++;

    // ---------------- Page Table Lookup ----------------
    pa_dram_t physical_address = page_table_translate(virtual_address, op);
    pa_dram_t physical_page_number = physical_address >> PAGE_SIZE_BITS;
    bool is_dirty = (op == OP_WRITE);

    // Inserir na L1
    insert_into_tlb(tlb_l1, TLB_L1_SIZE, virtual_page_number,
                    physical_page_number, is_dirty, false, get_time());
    
    // Inserir na L2
    insert_into_tlb(tlb_l2, TLB_L2_SIZE, virtual_page_number,
                physical_page_number, is_dirty, true, get_time());

    return physical_address;
}
