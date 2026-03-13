#include "flm_runtime.h"

#include <elf.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"

#include "target_swd.h"

static const char *TAG = "FLM_RT";

#define FLM_RT_MAX_SEGMENTS 12
#define FLM_RT_ALIGN_UP(v, a) (((v) + ((a)-1U)) & ~((a)-1U))
#define FLM_RT_ALIGN_DOWN(v, a) ((v) & ~((a)-1U))

static int flm_rt_is_ram_addr(uint32_t addr)
{
    return (addr >= 0x20000000UL && addr < 0x30000000UL);
}

typedef struct {
    uint32_t addr;
    uint32_t file_off;
    uint32_t file_size;
    uint32_t mem_size;
    uint32_t flags;
} flm_rt_seg_t;

typedef struct {
    uint8_t *blob;
    size_t blob_size;

    flm_rt_seg_t segs[FLM_RT_MAX_SEGMENTS];
    uint32_t seg_count;

    uint32_t init_addr;
    uint32_t uninit_addr;
    uint32_t erase_chip_addr;
    uint32_t erase_sector_addr;
    uint32_t program_page_addr;
    uint32_t breakpoint_addr;
    uint32_t breakpoint_generated;
    uint32_t reloc_enabled;
    uint32_t reloc_base;
    uint32_t image_min_addr;

    uint32_t prog_buf_addr;
    uint32_t prog_buf_size;
    uint32_t call_sp;
    uint32_t call_r9;
} flm_rt_ctx_t;

static uint32_t flm_rt_target_sram_end(void)
{
    uint32_t dbgmcu = 0;
    if (swd_read_dbgmcu_idcode(&dbgmcu) != 0) {
        return 0x20002000UL; // safe fallback
    }

    uint32_t dev_id = dbgmcu & 0x0FFFUL;
    switch (dev_id) {
    case 0x440: // STM32F0xx (e.g. F030C8)
        return 0x20002000UL; // 8 KB
    case 0x460: // STM32G0xx (e.g. G070RB)
        return 0x20009000UL; // 36 KB
    case 0x410:
    case 0x412:
    case 0x414:
    case 0x418: // STM32F1xx common
        return 0x20005000UL; // 20 KB common floor
    default:
        return 0x20008000UL; // generic fallback 32 KB
    }
}

static uint32_t flm_rt_target_core_hz(void)
{
    uint32_t dbgmcu = 0;
    if (swd_read_dbgmcu_idcode(&dbgmcu) != 0) {
        return 16000000UL; // safe default for modern STM32 families
    }

    uint32_t dev_id = dbgmcu & 0x0FFFUL;
    switch (dev_id) {
    case 0x440: // STM32F0xx
        return 8000000UL; // default HSI on most F0 after reset
    case 0x460: // STM32G0xx
        return 16000000UL; // default HSI16
    case 0x410:
    case 0x412:
    case 0x414:
    case 0x418: // STM32F1xx common
        return 8000000UL;
    default:
        return 16000000UL;
    }
}

static uint32_t flm_rt_reloc_sym(uint32_t sym, const flm_rt_ctx_t *ctx)
{
    if (!ctx || sym == 0) {
        return sym;
    }
    if (!ctx->reloc_enabled || flm_rt_is_ram_addr(sym) || sym < ctx->image_min_addr) {
        return sym;
    }
    return ctx->reloc_base + (sym - ctx->image_min_addr);
}

static void flm_rt_cleanup(flm_rt_ctx_t *ctx)
{
    if (!ctx) {
        return;
    }
    free(ctx->blob);
    memset(ctx, 0, sizeof(*ctx));
}

static int flm_rt_sym_match(const char *name, const char *target)
{
    if (!name || !target) {
        return 0;
    }
    if (strcmp(name, target) == 0) {
        return 1;
    }
    if (name[0] == '_' && strcmp(name + 1, target) == 0) {
        return 1;
    }
    return 0;
}

static int flm_rt_find_symbols(const uint8_t *blob, size_t blob_size, const Elf32_Ehdr *eh, flm_rt_ctx_t *ctx)
{
    if (!blob || !eh || !ctx) {
        return 0;
    }

    const Elf32_Shdr *sh = (const Elf32_Shdr *)(blob + eh->e_shoff);
    const Elf32_Shdr *symtab = NULL;
    for (uint16_t i = 0; i < eh->e_shnum; i++) {
        if (sh[i].sh_type == SHT_SYMTAB) {
            symtab = &sh[i];
            break;
        }
    }

    if (!symtab || symtab->sh_entsize != sizeof(Elf32_Sym) || symtab->sh_link >= eh->e_shnum) {
        return 0;
    }

    const Elf32_Shdr *strtab = &sh[symtab->sh_link];
    if ((size_t)symtab->sh_offset + symtab->sh_size > blob_size ||
        (size_t)strtab->sh_offset + strtab->sh_size > blob_size) {
        return 0;
    }

    const Elf32_Sym *symbols = (const Elf32_Sym *)(blob + symtab->sh_offset);
    uint32_t symbol_count = symtab->sh_size / symtab->sh_entsize;
    const char *names = (const char *)(blob + strtab->sh_offset);

    for (uint32_t i = 0; i < symbol_count; i++) {
        const Elf32_Sym *s = &symbols[i];
        if (s->st_name >= strtab->sh_size) {
            continue;
        }

        const char *name = names + s->st_name;
        if (flm_rt_sym_match(name, "Init")) {
            ctx->init_addr = s->st_value;
        } else if (flm_rt_sym_match(name, "UnInit")) {
            ctx->uninit_addr = s->st_value;
        } else if (flm_rt_sym_match(name, "EraseChip")) {
            ctx->erase_chip_addr = s->st_value;
        } else if (flm_rt_sym_match(name, "EraseSector")) {
            ctx->erase_sector_addr = s->st_value;
        } else if (flm_rt_sym_match(name, "ProgramPage")) {
            ctx->program_page_addr = s->st_value;
        } else if (flm_rt_sym_match(name, "BreakPoint")) {
            ctx->breakpoint_addr = s->st_value;
        }
    }

    return (ctx->init_addr != 0 && ctx->program_page_addr != 0) ? 1 : 0;
}

static esp_err_t flm_rt_parse_elf(FILE *algo_fp, flm_rt_ctx_t *ctx)
{
    if (!algo_fp || !ctx) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(ctx, 0, sizeof(*ctx));

    if (fseek(algo_fp, 0, SEEK_END) != 0) {
        return ESP_FAIL;
    }
    long file_len = ftell(algo_fp);
    if (file_len <= 0 || file_len > (1024L * 1024L)) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (fseek(algo_fp, 0, SEEK_SET) != 0) {
        return ESP_FAIL;
    }

    ctx->blob_size = (size_t)file_len;
    ctx->blob = (uint8_t *)malloc(ctx->blob_size);
    if (!ctx->blob) {
        return ESP_ERR_NO_MEM;
    }

    if (fread(ctx->blob, 1, ctx->blob_size, algo_fp) != ctx->blob_size) {
        return ESP_FAIL;
    }

    if (ctx->blob_size < sizeof(Elf32_Ehdr)) {
        return ESP_ERR_INVALID_SIZE;
    }

    const Elf32_Ehdr *eh = (const Elf32_Ehdr *)ctx->blob;
    if (memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0 ||
        eh->e_ident[EI_CLASS] != ELFCLASS32 ||
        eh->e_ident[EI_DATA] != ELFDATA2LSB) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (eh->e_phoff == 0 || eh->e_phentsize != sizeof(Elf32_Phdr) || eh->e_phnum == 0) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    size_t ph_size = (size_t)eh->e_phentsize * (size_t)eh->e_phnum;
    if ((size_t)eh->e_phoff + ph_size > ctx->blob_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    const Elf32_Phdr *ph = (const Elf32_Phdr *)(ctx->blob + eh->e_phoff);
    ESP_LOGI(TAG, "ELF phnum=%u", (unsigned)eh->e_phnum);

    uint32_t raw_addr[FLM_RT_MAX_SEGMENTS] = {0};
    uint32_t raw_file_off[FLM_RT_MAX_SEGMENTS] = {0};
    uint32_t raw_file_size[FLM_RT_MAX_SEGMENTS] = {0};
    uint32_t raw_mem_size[FLM_RT_MAX_SEGMENTS] = {0};
    uint32_t raw_flags[FLM_RT_MAX_SEGMENTS] = {0};
    uint32_t raw_count = 0;
    uint32_t any_ram_addr = 0;
    uint32_t min_raw = 0xFFFFFFFFUL;
    uint32_t max_raw_end = 0;

    for (uint16_t i = 0; i < eh->e_phnum && raw_count < FLM_RT_MAX_SEGMENTS; i++) {
        if (ph[i].p_type != PT_LOAD || ph[i].p_memsz == 0) {
            continue;
        }

        uint32_t raw = ph[i].p_paddr;
        if (raw == 0) {
            raw = ph[i].p_vaddr;
        }

        if ((size_t)ph[i].p_offset + (size_t)ph[i].p_filesz > ctx->blob_size) {
            return ESP_ERR_INVALID_SIZE;
        }

        raw_addr[raw_count] = raw;
        raw_file_off[raw_count] = ph[i].p_offset;
        raw_file_size[raw_count] = ph[i].p_filesz;
        raw_mem_size[raw_count] = ph[i].p_memsz;
        raw_flags[raw_count] = ph[i].p_flags;

        if (flm_rt_is_ram_addr(raw)) {
            any_ram_addr = 1;
        }
        if (raw < min_raw) {
            min_raw = raw;
        }
        if ((raw + ph[i].p_memsz) > max_raw_end) {
            max_raw_end = raw + ph[i].p_memsz;
        }

        raw_count++;
    }

    if (raw_count == 0) {
        ESP_LOGE(TAG, "No PT_LOAD segments");
        return ESP_ERR_NOT_FOUND;
    }

    if (!any_ram_addr) {
        // Relocatable FLM image (common in .flm packs): map into target SRAM.
        ctx->reloc_enabled = 1U;
        ctx->image_min_addr = min_raw;
        ctx->reloc_base = 0x20000100UL;
        ESP_LOGW(TAG,
                 "Relocatable FLM detected: min=0x%08" PRIX32 " max=0x%08" PRIX32 " -> base=0x%08" PRIX32,
                 min_raw,
                 max_raw_end,
                 ctx->reloc_base);
    }

    for (uint32_t i = 0; i < raw_count; i++) {
        uint32_t mapped = raw_addr[i];
        if (ctx->reloc_enabled) {
            mapped = ctx->reloc_base + (raw_addr[i] - ctx->image_min_addr);
        }

        if (!flm_rt_is_ram_addr(mapped)) {
            ESP_LOGW(TAG,
                     "Skip mapped seg[%" PRIu32 "]: raw=0x%08" PRIX32 " mapped=0x%08" PRIX32,
                     i,
                     raw_addr[i],
                     mapped);
            continue;
        }

        flm_rt_seg_t *seg = &ctx->segs[ctx->seg_count++];
        seg->addr = mapped;
        seg->file_off = raw_file_off[i];
        seg->file_size = raw_file_size[i];
        seg->mem_size = raw_mem_size[i];
        seg->flags = raw_flags[i];

        ESP_LOGI(TAG,
                 "Use seg[%" PRIu32 "]: raw=0x%08" PRIX32 " addr=0x%08" PRIX32 " off=0x%08" PRIX32 " file=0x%08" PRIX32 " mem=0x%08" PRIX32,
                 i,
                 raw_addr[i],
                 seg->addr,
                 seg->file_off,
                 seg->file_size,
                 seg->mem_size);
    }

    if (ctx->seg_count == 0) {
        ESP_LOGE(TAG, "No loadable PT_LOAD segments mapped to RAM");
        return ESP_ERR_NOT_FOUND;
    }

    if (eh->e_shoff == 0 || eh->e_shentsize != sizeof(Elf32_Shdr) || eh->e_shnum == 0) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    size_t sh_size = (size_t)eh->e_shentsize * (size_t)eh->e_shnum;
    if ((size_t)eh->e_shoff + sh_size > ctx->blob_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    if (!flm_rt_find_symbols(ctx->blob, ctx->blob_size, eh, ctx)) {
        ESP_LOGE(TAG, "Missing required symbols: Init/ProgramPage");
        return ESP_ERR_NOT_FOUND;
    }

    if (ctx->reloc_enabled) {
        ctx->init_addr = flm_rt_reloc_sym(ctx->init_addr, ctx);
        ctx->uninit_addr = flm_rt_reloc_sym(ctx->uninit_addr, ctx);
        ctx->erase_chip_addr = flm_rt_reloc_sym(ctx->erase_chip_addr, ctx);
        ctx->erase_sector_addr = flm_rt_reloc_sym(ctx->erase_sector_addr, ctx);
        ctx->program_page_addr = flm_rt_reloc_sym(ctx->program_page_addr, ctx);
        ctx->breakpoint_addr = flm_rt_reloc_sym(ctx->breakpoint_addr, ctx);
    }

    uint32_t top = 0;
    uint32_t base = 0xFFFFFFFFUL;
    uint32_t rw_base = 0xFFFFFFFFUL;
    for (uint32_t i = 0; i < ctx->seg_count; i++) {
        if (ctx->segs[i].addr < base) {
            base = ctx->segs[i].addr;
        }
        if ((ctx->segs[i].flags & PF_W) != 0U && ctx->segs[i].addr < rw_base) {
            rw_base = ctx->segs[i].addr;
        }
        uint32_t end = ctx->segs[i].addr + ctx->segs[i].mem_size;
        if (end > top) {
            top = end;
        }
    }
    if (base == 0xFFFFFFFFUL) {
        base = ctx->segs[0].addr;
    }

    uint32_t sram_end = flm_rt_target_sram_end();
    uint32_t work_start = FLM_RT_ALIGN_UP(top + 0x40U, 8U);
    if (work_start >= sram_end) {
        ESP_LOGE(TAG,
                 "No workspace RAM: top=0x%08" PRIX32 " sram_end=0x%08" PRIX32,
                 top,
                 sram_end);
        return ESP_ERR_NO_MEM;
    }

    uint32_t avail = sram_end - work_start;
    if (avail < 0x180U) {
        ESP_LOGE(TAG,
                 "Workspace too small: start=0x%08" PRIX32 " end=0x%08" PRIX32 " avail=0x%08" PRIX32,
                 work_start,
                 sram_end,
                 avail);
        return ESP_ERR_NO_MEM;
    }

    // Reserve upper 256 bytes for stack/guard; use remaining as program buffer.
    uint32_t max_prog = avail - 0x100U;
    if (max_prog > 2048U) {
        max_prog = 2048U;
    }
    if (max_prog < 128U) {
        max_prog = 128U;
    }

    ctx->prog_buf_addr = work_start;
    ctx->prog_buf_size = FLM_RT_ALIGN_DOWN(max_prog, 8U);
    ctx->call_sp = FLM_RT_ALIGN_DOWN(sram_end - 8U, 8U);
    ctx->call_r9 = (rw_base != 0xFFFFFFFFUL) ? rw_base : base;

    if (ctx->breakpoint_addr == 0) {
        // Fallback when FLM does not export BreakPoint symbol.
        ctx->breakpoint_addr = FLM_RT_ALIGN_UP(ctx->prog_buf_addr + ctx->prog_buf_size, 2U);
        ctx->breakpoint_generated = 1U;
        ESP_LOGW(TAG, "BreakPoint symbol missing, using generated BKPT at 0x%08" PRIX32, ctx->breakpoint_addr);
    }

    if (ctx->breakpoint_generated && ctx->breakpoint_addr >= sram_end) {
        ESP_LOGE(TAG,
                 "Generated BreakPoint out of SRAM: bp=0x%08" PRIX32 " sram_end=0x%08" PRIX32,
                 ctx->breakpoint_addr,
                 sram_end);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG,
             "Runtime symbols: Init=0x%08" PRIX32 " ProgramPage=0x%08" PRIX32 " EraseSector=0x%08" PRIX32 " BreakPoint=0x%08" PRIX32 " buf=0x%08" PRIX32 "(%" PRIu32 ") sp=0x%08" PRIX32 " r9=0x%08" PRIX32,
             ctx->init_addr,
             ctx->program_page_addr,
             ctx->erase_sector_addr,
             ctx->breakpoint_addr,
             ctx->prog_buf_addr,
             ctx->prog_buf_size,
             ctx->call_sp,
             ctx->call_r9);

    return ESP_OK;
}

static esp_err_t flm_rt_load_to_target(const flm_rt_ctx_t *ctx)
{
    if (!ctx || !ctx->blob || ctx->seg_count == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    for (uint32_t i = 0; i < ctx->seg_count; i++) {
        const flm_rt_seg_t *seg = &ctx->segs[i];
        if (seg->file_size > 0) {
            int ret = swd_mem_write_block(seg->addr, ctx->blob + seg->file_off, seg->file_size);
            if (ret != 0) {
                ESP_LOGE(TAG, "Load segment failed: idx=%" PRIu32 " ret=%d", i, ret);
                return ESP_FAIL;
            }
        }

        if (seg->mem_size > seg->file_size) {
            uint32_t zero_len = seg->mem_size - seg->file_size;
            uint8_t zero[64] = {0};
            uint32_t zoff = 0;
            while (zoff < zero_len) {
                uint32_t chunk = zero_len - zoff;
                if (chunk > sizeof(zero)) {
                    chunk = sizeof(zero);
                }
                int ret = swd_mem_write_block(seg->addr + seg->file_size + zoff, zero, chunk);
                if (ret != 0) {
                    ESP_LOGE(TAG, "Zero segment failed: idx=%" PRIu32 " ret=%d", i, ret);
                    return ESP_FAIL;
                }
                zoff += chunk;
            }
        }
    }

    if (ctx->breakpoint_generated) {
        static const uint8_t bkpt_thumb[2] = {0x00, 0xBE};
        int ret = swd_mem_write_block(ctx->breakpoint_addr, bkpt_thumb, sizeof(bkpt_thumb));
        if (ret != 0) {
            ESP_LOGE(TAG, "Write generated BKPT failed ret=%d", ret);
            return ESP_FAIL;
        }
    }

    return ESP_OK;
}

static esp_err_t flm_rt_call(const flm_rt_ctx_t *ctx,
                             uint32_t fn_addr,
                             uint32_t a0,
                             uint32_t a1,
                             uint32_t a2,
                             uint32_t a3,
                             uint32_t timeout_ms,
                             uint32_t *ret_out)
{
    uint32_t args[4] = {a0, a1, a2, a3};
    uint32_t ret = 0;
    int rc = swd_core_call(fn_addr, ctx->breakpoint_addr, ctx->call_sp, ctx->call_r9, args, &ret, timeout_ms);
    if (rc != 0) {
        ESP_LOGE(TAG, "FLM call failed fn=0x%08" PRIX32 " rc=%d", fn_addr, rc);
        return ESP_FAIL;
    }

    if (ret_out) {
        *ret_out = ret;
    }
    return ESP_OK;
}

static uint32_t flm_rt_sector_size_for_addr(const flm_info_t *info, uint32_t abs_addr)
{
    if (!info) {
        return 0;
    }

    uint32_t best_size = 0;
    uint32_t best_start = 0;

    for (uint32_t i = 0; i < info->sector_count; i++) {
        uint32_t rel = info->sectors[i].address;
        uint32_t sec_start = rel;
        if (rel < info->flash_size) {
            sec_start = info->flash_start + rel;
        }

        if (abs_addr >= sec_start && sec_start >= best_start) {
            best_start = sec_start;
            best_size = info->sectors[i].size;
        }
    }

    if (best_size == 0) {
        best_size = info->page_size;
    }
    if (best_size == 0) {
        best_size = 1024;
    }
    return best_size;
}

static esp_err_t flm_rt_erase_range(const flm_rt_ctx_t *ctx, const flm_info_t *info, uint32_t start, uint32_t size)
{
    if (size == 0) {
        return ESP_OK;
    }

    if (ctx->erase_chip_addr != 0 && start == info->flash_start && size >= info->flash_size) {
        uint32_t ret = 0;
        esp_err_t e = flm_rt_call(ctx, ctx->erase_chip_addr, 0, 0, 0, 0, info->erase_timeout_ms + 4000U, &ret);
        if (e == ESP_OK && ret == 0) {
            return ESP_OK;
        }
        ESP_LOGW(TAG, "EraseChip failed ret=%" PRIu32 ", fallback to EraseSector", ret);
    }

    if (ctx->erase_sector_addr == 0) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    uint32_t cur = start;
    uint32_t end = start + size;
    while (cur < end) {
        uint32_t sec_size = flm_rt_sector_size_for_addr(info, cur);
        uint32_t ret = 1;
        esp_err_t e = flm_rt_call(ctx, ctx->erase_sector_addr, cur, 0, 0, 0, info->erase_timeout_ms + 2000U, &ret);
        if (e != ESP_OK || ret != 0) {
            ESP_LOGE(TAG, "EraseSector failed at 0x%08" PRIX32 " ret=%" PRIu32, cur, ret);
            return ESP_FAIL;
        }
        cur += sec_size;
    }

    return ESP_OK;
}

static esp_err_t flm_rt_program_chunk(const flm_rt_ctx_t *ctx,
                                      const flm_info_t *info,
                                      uint32_t dst_addr,
                                      const uint8_t *data,
                                      uint32_t len)
{
    if (len == 0) {
        return ESP_OK;
    }

    int rc = swd_mem_write_block(ctx->prog_buf_addr, data, len);
    if (rc != 0) {
        ESP_LOGE(TAG, "Write page buffer failed rc=%d", rc);
        return ESP_FAIL;
    }

    uint32_t ret = 1;
    esp_err_t e = flm_rt_call(ctx,
                              ctx->program_page_addr,
                              dst_addr,
                              len,
                              ctx->prog_buf_addr,
                              0,
                              info->program_timeout_ms + 2000U,
                              &ret);
    if (e != ESP_OK || ret != 0) {
        ESP_LOGE(TAG, "ProgramPage failed at 0x%08" PRIX32 " ret=%" PRIu32, dst_addr, ret);
        return ESP_FAIL;
    }

    return ESP_OK;
}

static esp_err_t flm_rt_begin(const flm_rt_ctx_t *ctx, uint32_t base_addr, uint32_t fnc)
{
    uint32_t ret = 1;
    uint32_t clk_hz = flm_rt_target_core_hz();
    esp_err_t e = flm_rt_call(ctx, ctx->init_addr, base_addr, clk_hz, fnc, 0, 3000U, &ret);
    if (e != ESP_OK || ret != 0) {
        ESP_LOGE(TAG, "Init failed: ret=%" PRIu32, ret);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static void flm_rt_end(const flm_rt_ctx_t *ctx, uint32_t fnc)
{
    if (!ctx || ctx->uninit_addr == 0) {
        return;
    }
    uint32_t ret = 0;
    (void)flm_rt_call(ctx, ctx->uninit_addr, fnc, 0, 0, 0, 2000U, &ret);
}

esp_err_t flm_runtime_flash_buffer(FILE *algo_fp,
                                   const flm_info_t *info,
                                   uint32_t base_addr,
                                   const uint8_t *firmware,
                                   uint32_t firmware_size,
                                   uint32_t *out_written_bytes)
{
    if (!algo_fp || !info || !firmware || firmware_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    flm_rt_ctx_t ctx;
    esp_err_t err = flm_rt_parse_elf(algo_fp, &ctx);
    if (err != ESP_OK) {
        return err;
    }

    err = flm_rt_load_to_target(&ctx);
    if (err != ESP_OK) {
        flm_rt_cleanup(&ctx);
        return err;
    }

    err = flm_rt_begin(&ctx, base_addr, 2U);
    if (err != ESP_OK) {
        flm_rt_cleanup(&ctx);
        return err;
    }

    err = flm_rt_erase_range(&ctx, info, base_addr, firmware_size);
    if (err != ESP_OK) {
        flm_rt_end(&ctx, 2U);
        flm_rt_cleanup(&ctx);
        return err;
    }

    uint32_t page = info->page_size ? info->page_size : 256U;
    if (ctx.prog_buf_size > 0 && page > ctx.prog_buf_size) {
        page = ctx.prog_buf_size;
    }
    if (page > 4096U) {
        page = 4096U;
    }

    uint32_t off = 0;
    while (off < firmware_size) {
        uint32_t chunk = firmware_size - off;
        if (chunk > page) {
            chunk = page;
        }

        err = flm_rt_program_chunk(&ctx, info, base_addr + off, firmware + off, chunk);
        if (err != ESP_OK) {
            flm_rt_end(&ctx, 2U);
            flm_rt_cleanup(&ctx);
            return err;
        }

        off += chunk;
    }

    flm_rt_end(&ctx, 2U);
    flm_rt_cleanup(&ctx);

    if (out_written_bytes) {
        *out_written_bytes = firmware_size;
    }
    return ESP_OK;
}

esp_err_t flm_runtime_flash_stream(FILE *algo_fp,
                                   const flm_info_t *info,
                                   uint32_t base_addr,
                                   FILE *firmware_fp,
                                   uint32_t firmware_size,
                                   uint32_t *out_written_bytes)
{
    if (!algo_fp || !info || !firmware_fp || firmware_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    flm_rt_ctx_t ctx;
    esp_err_t err = flm_rt_parse_elf(algo_fp, &ctx);
    if (err != ESP_OK) {
        return err;
    }

    err = flm_rt_load_to_target(&ctx);
    if (err != ESP_OK) {
        flm_rt_cleanup(&ctx);
        return err;
    }

    err = flm_rt_begin(&ctx, base_addr, 2U);
    if (err != ESP_OK) {
        flm_rt_cleanup(&ctx);
        return err;
    }

    err = flm_rt_erase_range(&ctx, info, base_addr, firmware_size);
    if (err != ESP_OK) {
        flm_rt_end(&ctx, 2U);
        flm_rt_cleanup(&ctx);
        return err;
    }

    uint32_t page = info->page_size ? info->page_size : 256U;
    if (ctx.prog_buf_size > 0 && page > ctx.prog_buf_size) {
        page = ctx.prog_buf_size;
    }
    if (page > 4096U) {
        page = 4096U;
    }

    uint8_t *chunk_buf = (uint8_t *)malloc(page);
    if (!chunk_buf) {
        flm_rt_end(&ctx, 2U);
        flm_rt_cleanup(&ctx);
        return ESP_ERR_NO_MEM;
    }

    uint32_t off = 0;
    while (off < firmware_size) {
        uint32_t chunk = firmware_size - off;
        if (chunk > page) {
            chunk = page;
        }

        size_t n = fread(chunk_buf, 1, chunk, firmware_fp);
        if (n != chunk) {
            free(chunk_buf);
            flm_rt_end(&ctx, 2U);
            flm_rt_cleanup(&ctx);
            return ESP_FAIL;
        }

        err = flm_rt_program_chunk(&ctx, info, base_addr + off, chunk_buf, chunk);
        if (err != ESP_OK) {
            free(chunk_buf);
            flm_rt_end(&ctx, 2U);
            flm_rt_cleanup(&ctx);
            return err;
        }

        off += chunk;
    }

    free(chunk_buf);
    flm_rt_end(&ctx, 2U);
    flm_rt_cleanup(&ctx);

    if (out_written_bytes) {
        *out_written_bytes = firmware_size;
    }
    return ESP_OK;
}

esp_err_t flm_runtime_erase_chip(FILE *algo_fp, const flm_info_t *info)
{
    if (!algo_fp || !info) {
        return ESP_ERR_INVALID_ARG;
    }

    flm_rt_ctx_t ctx;
    esp_err_t err = flm_rt_parse_elf(algo_fp, &ctx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Parse .flm ELF failed: %s", esp_err_to_name(err));
        return err;
    }

    err = flm_rt_load_to_target(&ctx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Load to target failed: %s", esp_err_to_name(err));
        flm_rt_cleanup(&ctx);
        return err;
    }

    err = flm_rt_begin(&ctx, info->flash_start, 1U);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Init for erase failed: %s", esp_err_to_name(err));
        flm_rt_cleanup(&ctx);
        return err;
    }

    err = flm_rt_erase_range(&ctx, info, info->flash_start, info->flash_size);
    flm_rt_end(&ctx, 1U);
    flm_rt_cleanup(&ctx);
    ESP_LOGI(TAG, "Erase chip completed with result: %s", esp_err_to_name(err));
    return err;
}
