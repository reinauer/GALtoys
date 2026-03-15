/*
 * jedi.c - JED Interpreter: Convert non-standard JED files to standard JEDEC
 *
 * Supported input formats:
 *
 * Wellon programmer:
 *   - Header: "MODEL: Wellon"
 *   - Missing ETX (0x03) character
 *   - Missing file checksum after ETX
 *   - Token delimiter '*' after value (e.g., "QF0000*" vs "*QF0000")
 *   - Incorrect QF value (often 0)
 *
 * G540 programmer:
 *   - Header: "TITL:"
 *   - Missing STX/ETX
 *   - Token delimiter '*' after value
 *   - Single "L0000" followed by fuse data on separate lines
 *
 * Output: Standard JEDEC format compatible with minipro
 */

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STX 0x02
#define ETX 0x03
#define ROW_SIZE 32
#define MAX_FUSES 65536

typedef enum {
	FORMAT_UNKNOWN,
	FORMAT_WELLON,
	FORMAT_G540,
	FORMAT_UNIVERSAL,
	FORMAT_GALEP,
	FORMAT_STANDARD
} jed_format_t;

typedef struct {
	uint32_t addr;
	uint16_t len;
} fuse_row_t;

static const char *format_name(jed_format_t fmt)
{
	switch (fmt) {
	case FORMAT_WELLON:
		return "Wellon";
	case FORMAT_G540:
		return "G540";
	case FORMAT_UNIVERSAL:
		return "Universal Programmer";
	case FORMAT_GALEP:
		return "GALEP";
	case FORMAT_STANDARD:
		return "Standard JEDEC";
	default:
		return "Unknown";
	}
}

static int has_stx_prefix(const char *buffer, size_t size)
{
	size_t limit = size < 8 ? size : 8;

	return memchr(buffer, STX, limit) != NULL;
}

static jed_format_t detect_format(const char *buffer, size_t size)
{
	if (strstr(buffer, "MODEL: Wellon"))
		return FORMAT_WELLON;
	if (strstr(buffer, "TITL:"))
		return FORMAT_G540;
	if (strstr(buffer, "GALEP Jedec-File"))
		return FORMAT_GALEP;
	if (has_stx_prefix(buffer, size) &&
	    (strstr(buffer, "Fuse map produced by universal programmer") ||
	     strstr(buffer, "Fuse map produced by universal device programmer")))
		return FORMAT_UNIVERSAL;
	/* Check for standard format: STX present and *QF token */
	if (memchr(buffer, STX, size < 100 ? size : 100) && strstr(buffer, "*QF"))
		return FORMAT_STANDARD;
	return FORMAT_UNKNOWN;
}

static void usage(const char *prog)
{
	fprintf(stderr, "Usage: %s <input.jed> [output.jed]\n", prog);
	fprintf(stderr, "\nJED Interpreter - Convert non-standard JED to JEDEC format\n");
	fprintf(stderr, "\nSupported formats: Wellon, G540, Universal Programmer\n");
	fprintf(stderr, "\nIf output is omitted, writes to <input>_fixed.jed\n");
}

/*
 * Parse fuse data for Wellon format:
 * L000000 11111111111111111111111111111111*
 * L000032 11111111111111111111111111111111*
 */
static uint32_t parse_wellon_fuses(const char *buffer, uint8_t *fuses,
				   uint32_t max_fuses)
{
	const char *p = buffer;
	uint32_t max_addr = 0;

	while ((p = strstr(p, "L")) != NULL) {
		uint32_t addr;
		char *data_start;

		/* Skip if not at token boundary */
		if (p > buffer && isalpha((unsigned char)*(p - 1))) {
			p++;
			continue;
		}

		addr = strtoul(p + 1, &data_start, 10);
		if (data_start == p + 1) {
			p++;
			continue;
		}

		while (*data_start && isspace((unsigned char)*data_start))
			data_start++;

		while (*data_start == '0' || *data_start == '1') {
			if (addr >= max_fuses)
				break;
			fuses[addr] = (*data_start == '1') ? 1 : 0;
			if (addr >= max_addr)
				max_addr = addr + 1;
			addr++;
			data_start++;
			while (*data_start &&
			       (isspace((unsigned char)*data_start) ||
			        iscntrl((unsigned char)*data_start)) &&
			       *data_start != '*')
				data_start++;
		}
		p = data_start;
	}
	return max_addr;
}

/*
 * Parse fuse data for G540 format:
 * L0000
 * 11111111111111111111111111111111111111111111
 * 11111111111111111111111111111111111111111111*
 */
static uint32_t parse_g540_fuses(const char *buffer, uint8_t *fuses,
				 uint32_t max_fuses)
{
	const char *p;
	uint32_t addr = 0;

	p = strstr(buffer, "L0");
	if (!p)
		p = strstr(buffer, "L");
	if (!p)
		return 0;

	/* Skip to end of L line */
	while (*p && *p != '\n' && *p != '\r')
		p++;

	while (*p) {
		while (*p && (isspace((unsigned char)*p) ||
		       iscntrl((unsigned char)*p)))
			p++;

		if (!*p || *p == 'C' || *p == '*')
			break;

		while (*p == '0' || *p == '1') {
			if (addr >= max_fuses)
				return addr;
			fuses[addr++] = (*p == '1') ? 1 : 0;
			p++;
		}

		if (*p == '*')
			break;
	}
	return addr;
}

static int append_fuse_row(fuse_row_t **rows, size_t *count, size_t *capacity,
			   uint32_t addr, uint16_t len)
{
	fuse_row_t *grown;
	size_t new_capacity;

	if (*count == *capacity) {
		new_capacity = *capacity ? (*capacity * 2) : 128;
		grown = realloc(*rows, new_capacity * sizeof(**rows));
		if (!grown)
			return -1;
		*rows = grown;
		*capacity = new_capacity;
	}

	(*rows)[*count].addr = addr;
	(*rows)[*count].len = len;
	(*count)++;
	return 0;
}

static int parse_line_records(const char *buffer, uint8_t *fuses,
			      uint32_t max_fuses, fuse_row_t **rows_out,
			      size_t *row_count_out, uint32_t *qf_out)
{
	const char *line = buffer;
	fuse_row_t *rows = NULL;
	size_t count = 0, capacity = 0;
	uint32_t max_addr = 0;

	while (*line) {
		const char *cursor = line;
		const char *line_end;
		char *addr_end;
		uint32_t addr, row_addr;
		uint16_t bits = 0;

		while (*cursor == STX || *cursor == '\r' || *cursor == '\n')
			cursor++;
		if (!*cursor || *cursor == ETX)
			break;

		line_end = cursor;
		while (*line_end && *line_end != '\r' && *line_end != '\n' &&
		       *line_end != ETX)
			line_end++;

		if (*cursor == '*')
			cursor++;
		if (*cursor == 'L') {
			addr = strtoul(cursor + 1, &addr_end, 10);
			if (addr_end != cursor + 1) {
				while (addr_end < line_end &&
				       isspace((unsigned char)*addr_end))
					addr_end++;

				row_addr = addr;
				while (addr_end < line_end && *addr_end != '*') {
					if (*addr_end == '0' || *addr_end == '1') {
						if (addr >= max_fuses)
							break;
						fuses[addr++] = (*addr_end == '1') ? 1 : 0;
						bits++;
					}
					addr_end++;
				}

				if (bits > 0) {
					if (append_fuse_row(&rows, &count, &capacity,
							    row_addr, bits) < 0) {
						free(rows);
						return -1;
					}
					if (addr > max_addr)
						max_addr = addr;
				}
			}
		}

		line = line_end;
		while (*line == '\r' || *line == '\n')
			line++;
	}

	*rows_out = rows;
	*row_count_out = count;
	*qf_out = max_addr;
	return 0;
}

static int extract_stx_header_line(const char *buffer, char *name, size_t size)
{
	const char *p = strchr(buffer, STX);
	const char *end;
	size_t len;

	if (!p)
		return -1;

	p++;
	while (*p == '\r' || *p == '\n')
		p++;
	if (!*p)
		return -1;

	end = p;
	while (*end && *end != '\r' && *end != '\n' && *end != ETX)
		end++;

	len = end - p;
	if (len >= size)
		len = size - 1;
	if (len > 0)
		memcpy(name, p, len);
	name[len] = '\0';

	while (len > 0 && isspace((unsigned char)name[len - 1]))
		name[--len] = '\0';

	return 0;
}

static int extract_device_name(const char *buffer, char *name, size_t size,
			       jed_format_t format)
{
	const char *p = NULL, *end;
	size_t len;

	if (format == FORMAT_WELLON) {
		p = strstr(buffer, "TYPE:");
		if (p)
			p += 5;
	} else if (format == FORMAT_G540) {
		p = strstr(buffer, "TITL:");
		if (p)
			p += 5;
	} else if (format == FORMAT_UNIVERSAL || format == FORMAT_GALEP) {
		return extract_stx_header_line(buffer, name, size);
	}

	if (!p)
		return -1;

	while (*p && (*p == '\t' || *p == ' '))
		p++;

	end = strpbrk(p, "\r\n*");
	if (!end)
		return -1;

	len = end - p;
	if (len >= size)
		len = size - 1;
	strncpy(name, p, len);
	name[len] = '\0';

	while (len > 0 && isspace((unsigned char)name[len - 1]))
		name[--len] = '\0';

	return 0;
}

static uint32_t extract_qp(const char *buffer)
{
	const char *p = strstr(buffer, "QP");
	if (p)
		return strtoul(p + 2, NULL, 10);
	return 0;
}

static uint32_t infer_qp(const char *device_name)
{
	if (strstr(device_name, "22V10"))
		return 24;
	if (strstr(device_name, "20V8"))
		return 24;
	if (strstr(device_name, "16V8"))
		return 20;
	return 20;
}

int main(int argc, char *argv[])
{
	FILE *fin, *fout;
	char *buffer, *p;
	long file_size;
	uint8_t *fuses;
	uint32_t qf = 0, qp;
	uint16_t fuse_checksum = 0, file_checksum = 0;
	size_t i;
	char device_name[64] = "Unknown";
	char out_buffer[1024 * 1024];
	char *out_ptr;
	char out_filename[256];
	jed_format_t format;
	fuse_row_t *rows = NULL;
	size_t row_count = 0;

	if (argc < 2 || argc > 3) {
		usage(argv[0]);
		return 1;
	}

	if (argc == 3) {
		strncpy(out_filename, argv[2], sizeof(out_filename) - 1);
		out_filename[sizeof(out_filename) - 1] = '\0';
	} else {
		strncpy(out_filename, argv[1], sizeof(out_filename) - 12);
		p = strrchr(out_filename, '.');
		if (p)
			*p = '\0';
		strcat(out_filename, "_fixed.jed");
	}

	fin = fopen(argv[1], "rb");
	if (!fin) {
		fprintf(stderr, "Cannot open: %s\n", argv[1]);
		return 1;
	}

	fseek(fin, 0, SEEK_END);
	file_size = ftell(fin);
	fseek(fin, 0, SEEK_SET);

	buffer = calloc(1, file_size + 1);
	if (!buffer) {
		fprintf(stderr, "Out of memory\n");
		fclose(fin);
		return 1;
	}
	if (fread(buffer, 1, file_size, fin) != (size_t)file_size) {
		fprintf(stderr, "Failed to read input file\n");
		free(buffer);
		fclose(fin);
		return 1;
	}
	fclose(fin);

	format = detect_format(buffer, file_size);
	printf("Format: %s\n", format_name(format));

	if (format == FORMAT_STANDARD || format == FORMAT_GALEP) {
		printf("File is already %s format\n",
		       format == FORMAT_GALEP ? "GALEP-compatible JEDEC" :
						"standard JEDEC");
		free(buffer);
		return 0;
	}

	fuses = calloc(1, MAX_FUSES);
	if (!fuses) {
		fprintf(stderr, "Out of memory\n");
		free(buffer);
		return 1;
	}

	if (extract_device_name(buffer, device_name, sizeof(device_name), format) == 0)
		printf("Device: %s\n", device_name);

	qp = extract_qp(buffer);
	if (!qp)
		qp = infer_qp(device_name);
	printf("Pins: %u\n", qp);

	switch (format) {
	case FORMAT_UNIVERSAL:
		if (parse_line_records(buffer, fuses, MAX_FUSES, &rows, &row_count,
				       &qf) < 0) {
			fprintf(stderr, "Out of memory\n");
			free(fuses);
			free(buffer);
			return 1;
		}
		break;
	case FORMAT_G540:
		qf = parse_g540_fuses(buffer, fuses, MAX_FUSES);
		break;
	default:
		qf = parse_wellon_fuses(buffer, fuses, MAX_FUSES);
		if (qf == 0)
			qf = parse_g540_fuses(buffer, fuses, MAX_FUSES);
		break;
	}

	if (qf == 0) {
		fprintf(stderr, "No fuse data found\n");
		free(rows);
		free(fuses);
		free(buffer);
		return 1;
	}

	printf("Fuses: %u\n", qf);

	for (i = 0; i < qf; i++)
		if (fuses[i])
			fuse_checksum += (0x01 << (i & 0x07));
	printf("Checksum: %04X\n", fuse_checksum);

	out_ptr = out_buffer;
	if (format == FORMAT_UNIVERSAL) {
		out_ptr += sprintf(out_ptr, "%c\r\n%s\r\n\r\nQF%u*\r\nF0*\r\nG0*\r\n\r\n",
				   STX, device_name, qf);
		for (i = 0; i < row_count; i++) {
			uint32_t addr = rows[i].addr;
			uint16_t len = rows[i].len;
			uint16_t j;

			out_ptr += sprintf(out_ptr, "L%05u ", addr);
			for (j = 0; j < len; j++)
				*out_ptr++ = fuses[addr + j] ? '1' : '0';
			out_ptr += sprintf(out_ptr, "*\r\n");
		}
		out_ptr += sprintf(out_ptr, "C%04X*\r\n%c", fuse_checksum, ETX);
	} else {
		out_ptr += sprintf(out_ptr,
			"%c\r\nDevice: %s\r\n\r\n"
			"NOTE: Converted from %s format by JEDi\r\n\r\n"
			"*QP%u\r\n*QF%u\r\n*F0\r\n*G0\r\n\r\n",
			STX, device_name, format_name(format), qp, qf);

		for (i = 0; i < qf; i++) {
			if ((i % ROW_SIZE) == 0)
				out_ptr += sprintf(out_ptr, "%s*L%05u ",
						  i ? "\r\n" : "", (uint32_t)i);
			*out_ptr++ = fuses[i] ? '1' : '0';
		}

		/*
		 * Terminate the final C field before ETX; strict parsers reject
		 * it otherwise.
		 */
		out_ptr += sprintf(out_ptr, "\r\n*C%04X\r\n*%c", fuse_checksum, ETX);
	}

	for (p = out_buffer; p < out_ptr; p++)
		file_checksum += (uint8_t)*p;

	sprintf(out_ptr, "%04X\r\n", file_checksum);

	fout = fopen(out_filename, "wb");
	if (!fout) {
		fprintf(stderr, "Cannot create: %s\n", out_filename);
		free(rows);
		free(fuses);
		free(buffer);
		return 1;
	}
	fputs(out_buffer, fout);
	fclose(fout);

	printf("Wrote: %s\n", out_filename);

	free(rows);
	free(fuses);
	free(buffer);
	return 0;
}
