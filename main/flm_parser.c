#include "flm_parser.h"

#include <elf.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"

static const char *TAG = "FLM";

typedef struct
{
	uint16_t version;
	uint8_t name[128];
	uint16_t device_type;
	uint32_t flash_start;
	uint32_t flash_size;
	uint32_t page_size;
	uint32_t reserved;
	uint8_t erased_value;
	uint8_t pad[3];
	uint32_t program_timeout_ms;
	uint32_t erase_timeout_ms;
} __attribute__((packed)) flm_flash_device_head_t;

typedef struct
{
	uint32_t size;
	uint32_t address;
} __attribute__((packed)) flm_flash_sector_pair_t;

static size_t flm_safe_strnlen(const char *s, size_t max_len)
{
	size_t n = 0;
	while (n < max_len && s[n] != '\0')
	{
		n++;
	}
	return n;
}

static int flm_vaddr_to_offset(const uint8_t *blob, size_t blob_size, uint32_t vaddr, size_t *out_offset)
{
	if (!blob || !out_offset || blob_size < sizeof(Elf32_Ehdr))
	{
		return 0;
	}

	const Elf32_Ehdr *eh = (const Elf32_Ehdr *)blob;
	if (eh->e_shoff == 0 || eh->e_shentsize != sizeof(Elf32_Shdr))
	{
		return 0;
	}

	size_t sh_table_size = (size_t)eh->e_shentsize * (size_t)eh->e_shnum;
	if ((size_t)eh->e_shoff + sh_table_size > blob_size)
	{
		return 0;
	}

	const Elf32_Shdr *sh = (const Elf32_Shdr *)(blob + eh->e_shoff);
	for (uint16_t i = 0; i < eh->e_shnum; i++)
	{
		uint32_t sh_addr = sh[i].sh_addr;
		uint32_t sh_size = sh[i].sh_size;
		if (sh_size == 0)
		{
			continue;
		}

		uint32_t sh_end = sh_addr + sh_size;
		if (vaddr >= sh_addr && vaddr < sh_end)
		{
			uint32_t delta = vaddr - sh_addr;
			size_t off = (size_t)sh[i].sh_offset + (size_t)delta;
			if (off >= blob_size)
			{
				return 0;
			}
			*out_offset = off;
			return 1;
		}
	}

	return 0;
}

esp_err_t flm_parse_from_file(FILE *fp, const char *name_hint, flm_info_t *out_info)
{
	if (!fp || !out_info)
	{
		return ESP_ERR_INVALID_ARG;
	}

	memset(out_info, 0, sizeof(*out_info));
	if (name_hint)
	{
		strncpy(out_info->source_name, name_hint, sizeof(out_info->source_name) - 1U);
	}

	if (fseek(fp, 0, SEEK_END) != 0)
	{
		return ESP_FAIL;
	}

	long file_len = ftell(fp);
	if (file_len <= 0 || file_len > (1024L * 1024L))
	{
		return ESP_ERR_INVALID_SIZE;
	}

	if (fseek(fp, 0, SEEK_SET) != 0)
	{
		return ESP_FAIL;
	}

	size_t blob_size = (size_t)file_len;
	uint8_t *blob = (uint8_t *)malloc(blob_size);
	if (!blob)
	{
		return ESP_ERR_NO_MEM;
	}

	esp_err_t rc = ESP_FAIL;
	do
	{
		if (fread(blob, 1, blob_size, fp) != blob_size)
		{
			rc = ESP_FAIL;
			break;
		}

		if (blob_size < sizeof(Elf32_Ehdr))
		{
			rc = ESP_ERR_INVALID_SIZE;
			break;
		}

		const Elf32_Ehdr *eh = (const Elf32_Ehdr *)blob;
		if (memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0 ||
			eh->e_ident[EI_CLASS] != ELFCLASS32 ||
			eh->e_ident[EI_DATA] != ELFDATA2LSB)
		{
			rc = ESP_ERR_NOT_SUPPORTED;
			break;
		}

		if (eh->e_shoff == 0 || eh->e_shentsize != sizeof(Elf32_Shdr) || eh->e_shnum == 0)
		{
			rc = ESP_ERR_INVALID_RESPONSE;
			break;
		}

		size_t sh_table_size = (size_t)eh->e_shentsize * (size_t)eh->e_shnum;
		if ((size_t)eh->e_shoff + sh_table_size > blob_size)
		{
			rc = ESP_ERR_INVALID_SIZE;
			break;
		}

		const Elf32_Shdr *sh = (const Elf32_Shdr *)(blob + eh->e_shoff);
		const Elf32_Shdr *symtab = NULL;
		for (uint16_t i = 0; i < eh->e_shnum; i++)
		{
			if (sh[i].sh_type == SHT_SYMTAB)
			{
				symtab = &sh[i];
				break;
			}
		}

		if (!symtab || symtab->sh_entsize != sizeof(Elf32_Sym) || symtab->sh_link >= eh->e_shnum)
		{
			rc = ESP_ERR_NOT_FOUND;
			break;
		}

		const Elf32_Shdr *strtab = &sh[symtab->sh_link];
		if ((size_t)symtab->sh_offset + symtab->sh_size > blob_size ||
			(size_t)strtab->sh_offset + strtab->sh_size > blob_size)
		{
			rc = ESP_ERR_INVALID_SIZE;
			break;
		}

		const Elf32_Sym *symbols = (const Elf32_Sym *)(blob + symtab->sh_offset);
		uint32_t symbol_count = symtab->sh_size / symtab->sh_entsize;
		const char *sym_names = (const char *)(blob + strtab->sh_offset);

		uint32_t flash_device_vaddr = 0;
		for (uint32_t i = 0; i < symbol_count; i++)
		{
			const Elf32_Sym *s = &symbols[i];
			if (s->st_name >= strtab->sh_size)
			{
				continue;
			}

			const char *name = sym_names + s->st_name;
			if (strcmp(name, "FlashDevice") == 0)
			{
				flash_device_vaddr = s->st_value;
				break;
			}
		}

		if (flash_device_vaddr == 0)
		{
			rc = ESP_ERR_NOT_FOUND;
			break;
		}

		size_t flash_device_off = 0;
		if (!flm_vaddr_to_offset(blob, blob_size, flash_device_vaddr, &flash_device_off))
		{
			rc = ESP_ERR_INVALID_RESPONSE;
			break;
		}

		size_t min_head_size = sizeof(flm_flash_device_head_t);
		if (flash_device_off + min_head_size > blob_size)
		{
			rc = ESP_ERR_INVALID_SIZE;
			break;
		}

		const flm_flash_device_head_t *head = (const flm_flash_device_head_t *)(blob + flash_device_off);
		out_info->version = head->version;
		out_info->device_type = head->device_type;
		out_info->flash_start = head->flash_start;
		out_info->flash_size = head->flash_size;
		out_info->page_size = head->page_size;
		out_info->erased_value = head->erased_value;
		out_info->program_timeout_ms = head->program_timeout_ms;
		out_info->erase_timeout_ms = head->erase_timeout_ms;

		size_t dev_name_len = flm_safe_strnlen((const char *)head->name, sizeof(head->name));
		memcpy(out_info->device_name, head->name, dev_name_len);
		out_info->device_name[dev_name_len] = '\0';

		size_t sector_off = flash_device_off + sizeof(flm_flash_device_head_t);
		out_info->sector_count = 0;
		for (uint32_t i = 0; i < FLM_MAX_SECTORS; i++)
		{
			if (sector_off + sizeof(flm_flash_sector_pair_t) > blob_size)
			{
				break;
			}

			const flm_flash_sector_pair_t *sec = (const flm_flash_sector_pair_t *)(blob + sector_off);
			if (sec->size == 0xFFFFFFFFUL || sec->address == 0xFFFFFFFFUL)
			{
				break;
			}

			out_info->sectors[i].size = sec->size;
			out_info->sectors[i].address = sec->address;
			out_info->sector_count++;
			sector_off += sizeof(flm_flash_sector_pair_t);
		}

		if (out_info->flash_start == 0 || out_info->flash_size == 0)
		{
			rc = ESP_ERR_INVALID_RESPONSE;
			break;
		}

		ESP_LOGI(TAG,
				 "FLM parsed: name='%s' flash=0x%08" PRIX32 " size=%" PRIu32 " page=%" PRIu32,
				 out_info->device_name,
				 out_info->flash_start,
				 out_info->flash_size,
				 out_info->page_size);

		rc = ESP_OK;
	} while (0);

	free(blob);
	return rc;
}
