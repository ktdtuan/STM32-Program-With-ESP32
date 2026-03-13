#include "hex2bin.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"

#define HEX_LINE_MAX 600
#define FILL_CHUNK_SIZE 256

static const char *TAG = "HEX2BIN";

typedef struct hex_seg {
    uint32_t addr;
    uint16_t len;
    uint8_t *data;
    struct hex_seg *next;
} hex_seg_t;

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return 10 + (c - 'a');
    }
    if (c >= 'A' && c <= 'F') {
        return 10 + (c - 'A');
    }
    return -1;
}

static int hex_byte(const char *p, uint8_t *out)
{
    int hi = hex_nibble(p[0]);
    int lo = hex_nibble(p[1]);
    if (hi < 0 || lo < 0) {
        return -1;
    }
    *out = (uint8_t)((hi << 4) | lo);
    return 0;
}

static void free_segments(hex_seg_t *head)
{
    while (head) {
        hex_seg_t *next = head->next;
        free(head->data);
        free(head);
        head = next;
    }
}

static esp_err_t parse_hex_line(const char *line,
                           size_t len,
                           uint8_t *out_reclen,
                           uint16_t *out_recaddr,
                           uint8_t *out_rectype,
                           uint8_t *out_data)
{
    if (!line || !out_reclen || !out_recaddr || !out_rectype || !out_data)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (line[0] != ':' || len < 11)
    {
        return ESP_ERR_INVALID_RESPONSE;
    }

    uint8_t reclen = 0;
    uint8_t addr_hi = 0;
    uint8_t addr_lo = 0;
    uint8_t rectype = 0;
    if (hex_byte(&line[1], &reclen) != 0 ||
        hex_byte(&line[3], &addr_hi) != 0 ||
        hex_byte(&line[5], &addr_lo) != 0 ||
        hex_byte(&line[7], &rectype) != 0)
    {
        return ESP_ERR_INVALID_RESPONSE;
    }

    size_t expected_chars = (size_t)11 + ((size_t)reclen * 2);
    if (len < expected_chars)
    {
        return ESP_ERR_INVALID_RESPONSE;
    }

    uint8_t sum = (uint8_t)(reclen + addr_hi + addr_lo + rectype);
    for (uint8_t i = 0; i < reclen; ++i)
    {
        if (hex_byte(&line[9 + (i * 2)], &out_data[i]) != 0)
        {
            return ESP_ERR_INVALID_RESPONSE;
        }
        sum = (uint8_t)(sum + out_data[i]);
    }

    uint8_t checksum = 0;
    if (hex_byte(&line[9 + (reclen * 2)], &checksum) != 0)
    {
        return ESP_ERR_INVALID_RESPONSE;
    }
    sum = (uint8_t)(sum + checksum);
    if (sum != 0)
    {
        return ESP_ERR_INVALID_CRC;
    }

    *out_reclen = reclen;
    *out_recaddr = (uint16_t)(((uint16_t)addr_hi << 8) | addr_lo);
    *out_rectype = rectype;
    return ESP_OK;
}

static esp_err_t scan_hex_bounds(FILE *hex_fp, uint32_t *out_min_addr, uint32_t *out_max_addr)
{
    if (!hex_fp || !out_min_addr || !out_max_addr)
    {
        return ESP_ERR_INVALID_ARG;
    }

    rewind(hex_fp);

    char line[HEX_LINE_MAX];
    uint8_t data[256];
    uint32_t upper = 0;
    uint32_t min_addr = UINT32_MAX;
    uint32_t max_addr = 0;
    int have_data = 0;
    uint32_t line_no = 0;

    while (fgets(line, sizeof(line), hex_fp))
    {
        line_no++;
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
        {
            line[--len] = '\0';
        }
        if (len == 0)
        {
            continue;
        }

        uint8_t reclen = 0;
        uint16_t recaddr = 0;
        uint8_t rectype = 0;
        esp_err_t lret = parse_hex_line(line, len, &reclen, &recaddr, &rectype, data);
        if (lret != ESP_OK)
        {
            ESP_LOGE(TAG, "HEX parse failed at line=%" PRIu32 " err=%s", line_no, esp_err_to_name(lret));
            return lret;
        }

        if (rectype == 0x00)
        {
            if (reclen > 0)
            {
                uint32_t abs_addr = upper + recaddr;
                uint32_t end_addr = abs_addr + reclen;
                if (abs_addr < min_addr)
                {
                    min_addr = abs_addr;
                }
                if (end_addr > max_addr)
                {
                    max_addr = end_addr;
                }
                have_data = 1;
            }
        }
        else if (rectype == 0x01)
        {
            break;
        }
        else if (rectype == 0x02)
        {
            if (reclen != 2)
            {
                ESP_LOGE(TAG, "Invalid HEX type02 length at line=%" PRIu32, line_no);
                return ESP_ERR_INVALID_RESPONSE;
            }
            uint16_t seg = (uint16_t)(((uint16_t)data[0] << 8) | data[1]);
            upper = ((uint32_t)seg) << 4;
        }
        else if (rectype == 0x04)
        {
            if (reclen != 2)
            {
                ESP_LOGE(TAG, "Invalid HEX type04 length at line=%" PRIu32, line_no);
                return ESP_ERR_INVALID_RESPONSE;
            }
            uint16_t lin = (uint16_t)(((uint16_t)data[0] << 8) | data[1]);
            upper = ((uint32_t)lin) << 16;
        }
    }

    if (!have_data || min_addr >= max_addr)
    {
        ESP_LOGE(TAG, "No HEX data records found");
        return ESP_ERR_NOT_FOUND;
    }

    *out_min_addr = min_addr;
    *out_max_addr = max_addr;
    return ESP_OK;
}

static esp_err_t parse_hex_segments(FILE *hex_fp,
                                    hex_seg_t **out_head,
                                    uint32_t *out_min_addr,
                                    uint32_t *out_max_addr)
{
    if (!hex_fp || !out_head || !out_min_addr || !out_max_addr) {
        return ESP_ERR_INVALID_ARG;
    }

    rewind(hex_fp);

    char line[HEX_LINE_MAX];
    uint32_t upper = 0;
    uint32_t min_addr = UINT32_MAX;
    uint32_t max_addr = 0;
    int have_data = 0;

    hex_seg_t *head = NULL;
    hex_seg_t *tail = NULL;

    while (fgets(line, sizeof(line), hex_fp)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = '\0';
        }
        if (len == 0) {
            continue;
        }
        if (line[0] != ':') {
            free_segments(head);
            return ESP_ERR_INVALID_RESPONSE;
        }

        if (len < 11) {
            free_segments(head);
            return ESP_ERR_INVALID_RESPONSE;
        }

        uint8_t reclen = 0;
        uint8_t addr_hi = 0;
        uint8_t addr_lo = 0;
        uint8_t rectype = 0;
        if (hex_byte(&line[1], &reclen) != 0 ||
            hex_byte(&line[3], &addr_hi) != 0 ||
            hex_byte(&line[5], &addr_lo) != 0 ||
            hex_byte(&line[7], &rectype) != 0) {
            free_segments(head);
            return ESP_ERR_INVALID_RESPONSE;
        }

        size_t expected_chars = (size_t)11 + ((size_t)reclen * 2);
        if (len < expected_chars) {
            free_segments(head);
            return ESP_ERR_INVALID_RESPONSE;
        }

        uint8_t data[256];

        uint8_t sum = (uint8_t)(reclen + addr_hi + addr_lo + rectype);
        for (uint8_t i = 0; i < reclen; ++i) {
            if (hex_byte(&line[9 + (i * 2)], &data[i]) != 0) {
                free_segments(head);
                return ESP_ERR_INVALID_RESPONSE;
            }
            sum = (uint8_t)(sum + data[i]);
        }

        uint8_t checksum = 0;
        if (hex_byte(&line[9 + (reclen * 2)], &checksum) != 0) {
            free_segments(head);
            return ESP_ERR_INVALID_RESPONSE;
        }
        sum = (uint8_t)(sum + checksum);
        if (sum != 0) {
            free_segments(head);
            return ESP_ERR_INVALID_CRC;
        }

        uint16_t recaddr = (uint16_t)(((uint16_t)addr_hi << 8) | addr_lo);
        if (rectype == 0x00) {
            if (reclen > 0) {
                uint32_t abs_addr = upper + recaddr;
                uint32_t end_addr = abs_addr + reclen;

                if (abs_addr < min_addr) {
                    min_addr = abs_addr;
                }
                if (end_addr > max_addr) {
                    max_addr = end_addr;
                }
                have_data = 1;

                hex_seg_t *seg = (hex_seg_t *)calloc(1, sizeof(hex_seg_t));
                if (!seg) {
                    free_segments(head);
                    return ESP_ERR_NO_MEM;
                }
                seg->data = (uint8_t *)malloc(reclen);
                if (!seg->data) {
                    free(seg);
                    free_segments(head);
                    return ESP_ERR_NO_MEM;
                }

                seg->addr = abs_addr;
                seg->len = reclen;
                memcpy(seg->data, data, reclen);

                if (!head) {
                    head = seg;
                    tail = seg;
                } else {
                    tail->next = seg;
                    tail = seg;
                }
            }
        } else if (rectype == 0x01) {
            break;
        } else if (rectype == 0x02) {
            if (reclen != 2) {
                free_segments(head);
                return ESP_ERR_INVALID_RESPONSE;
            }
            uint16_t seg = (uint16_t)(((uint16_t)data[0] << 8) | data[1]);
            upper = ((uint32_t)seg) << 4;
        } else if (rectype == 0x04) {
            if (reclen != 2) {
                free_segments(head);
                return ESP_ERR_INVALID_RESPONSE;
            }
            uint16_t lin = (uint16_t)(((uint16_t)data[0] << 8) | data[1]);
            upper = ((uint32_t)lin) << 16;
        }
    }

    if (!have_data || min_addr >= max_addr) {
        free_segments(head);
        return ESP_ERR_NOT_FOUND;
    }

    *out_head = head;
    *out_min_addr = min_addr;
    *out_max_addr = max_addr;
    return ESP_OK;
}

esp_err_t hex2bin_stream(FILE *hex_fp, FILE *bin_fp, uint32_t *out_base_addr, uint32_t *out_size)
{
    if (!hex_fp || !bin_fp || !out_base_addr || !out_size) {
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t min_addr = 0;
    uint32_t max_addr = 0;
    esp_err_t pret = scan_hex_bounds(hex_fp, &min_addr, &max_addr);
    if (pret != ESP_OK)
    {
        return pret;
    }

    uint32_t total_size = max_addr - min_addr;
    uint8_t fill[FILL_CHUNK_SIZE];
    memset(fill, 0xFF, sizeof(fill));

    rewind(bin_fp);
    uint32_t written = 0;
    while (written < total_size)
    {
        uint32_t chunk = total_size - written;
        if (chunk > sizeof(fill))
        {
            chunk = sizeof(fill);
        }
        if (fwrite(fill, 1, chunk, bin_fp) != chunk)
        {
            ESP_LOGE(TAG, "Pre-fill output failed at %" PRIu32 "/%" PRIu32, written, total_size);
            return ESP_FAIL;
        }
        written += chunk;
    }

    rewind(hex_fp);
    char line[HEX_LINE_MAX];
    uint8_t data[256];
    uint32_t upper = 0;
    uint32_t line_no = 0;
    while (fgets(line, sizeof(line), hex_fp))
    {
        line_no++;
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
        {
            line[--len] = '\0';
        }
        if (len == 0)
        {
            continue;
        }

        uint8_t reclen = 0;
        uint16_t recaddr = 0;
        uint8_t rectype = 0;
        esp_err_t lret = parse_hex_line(line, len, &reclen, &recaddr, &rectype, data);
        if (lret != ESP_OK)
        {
            ESP_LOGE(TAG, "HEX replay failed at line=%" PRIu32 " err=%s", line_no, esp_err_to_name(lret));
            return lret;
        }

        if (rectype == 0x00 && reclen > 0)
        {
            uint32_t abs_addr = upper + recaddr;
            uint32_t off = abs_addr - min_addr;
            if (fseek(bin_fp, (long)off, SEEK_SET) != 0)
            {
                ESP_LOGE(TAG, "Seek write offset failed at line=%" PRIu32 " off=0x%08" PRIX32, line_no, off);
                return ESP_FAIL;
            }
            if (fwrite(data, 1, reclen, bin_fp) != reclen)
            {
                ESP_LOGE(TAG, "Write data failed at line=%" PRIu32 " len=%u", line_no, (unsigned)reclen);
                return ESP_FAIL;
            }
        }
        else if (rectype == 0x01)
        {
            break;
        }
        else if (rectype == 0x02)
        {
            if (reclen != 2)
            {
                ESP_LOGE(TAG, "Invalid HEX type02 length at replay line=%" PRIu32, line_no);
                return ESP_ERR_INVALID_RESPONSE;
            }
            uint16_t seg = (uint16_t)(((uint16_t)data[0] << 8) | data[1]);
            upper = ((uint32_t)seg) << 4;
        }
        else if (rectype == 0x04)
        {
            if (reclen != 2)
            {
                ESP_LOGE(TAG, "Invalid HEX type04 length at replay line=%" PRIu32, line_no);
                return ESP_ERR_INVALID_RESPONSE;
            }
            uint16_t lin = (uint16_t)(((uint16_t)data[0] << 8) | data[1]);
            upper = ((uint32_t)lin) << 16;
        }
    }

    fflush(bin_fp);
    if (fseek(bin_fp, 0, SEEK_SET) != 0)
    {
        ESP_LOGE(TAG, "rewind output failed");
        return ESP_FAIL;
    }

    *out_base_addr = min_addr;
    *out_size = total_size;
    ESP_LOGI(TAG, "HEX->BIN stream done: base=0x%08" PRIX32 " size=%" PRIu32, min_addr, total_size);
    return ESP_OK;
}

esp_err_t hex2bin_to_buffer(FILE *hex_fp, uint8_t **out_buf, uint32_t *out_base_addr, uint32_t *out_size)
{
    if (!hex_fp || !out_buf || !out_base_addr || !out_size) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_buf = NULL;
    *out_base_addr = 0;
    *out_size = 0;

    hex_seg_t *head = NULL;
    uint32_t min_addr = 0;
    uint32_t max_addr = 0;
    esp_err_t pret = parse_hex_segments(hex_fp, &head, &min_addr, &max_addr);
    if (pret != ESP_OK) {
        return pret;
    }

    uint32_t total_size = max_addr - min_addr;
    uint8_t *buf = (uint8_t *)malloc(total_size);
    if (!buf) {
        free_segments(head);
        return ESP_ERR_NO_MEM;
    }

    memset(buf, 0xFF, total_size);
    for (hex_seg_t *seg = head; seg != NULL; seg = seg->next) {
        uint32_t off = seg->addr - min_addr;
        memcpy(buf + off, seg->data, seg->len);
    }

    free_segments(head);
    *out_buf = buf;
    *out_base_addr = min_addr;
    *out_size = total_size;
    return ESP_OK;
}
