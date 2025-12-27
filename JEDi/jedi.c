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
	FORMAT_STANDARD
} jed_format_t;

static const char *format_name(jed_format_t fmt)
{
	switch (fmt) {
	case FORMAT_WELLON:
		return "Wellon";
	case FORMAT_G540:
		return "G540";
	case FORMAT_STANDARD:
		return "Standard JEDEC";
	default:
		return "Unknown";
	}
}

static jed_format_t detect_format(const char *buffer, size_t size)
{
	if (strstr(buffer, "MODEL: Wellon"))
		return FORMAT_WELLON;
	if (strstr(buffer, "TITL:"))
		return FORMAT_G540;
	/* Check for standard format: STX present and *QF token */
	if (memchr(buffer, STX, size < 100 ? size : 100) && strstr(buffer, "*QF"))
		return FORMAT_STANDARD;
	return FORMAT_UNKNOWN;
}

static void usage(const char *prog)
{
	fprintf(stderr, "Usage: %s <input.jed> [output.jed]\n", prog);
	fprintf(stderr, "\nJED Interpreter - Convert non-standard JED to JEDEC format\n");
	fprintf(stderr, "\nSupported formats: Wellon, G540\n");
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

	if (format == FORMAT_STANDARD) {
		printf("File is already standard JEDEC format\n");
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
		qp = 20; /* Default for GAL16V8 */
	printf("Pins: %u\n", qp);

	switch (format) {
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

	out_ptr += sprintf(out_ptr, "\r\n*C%04X\r\n%c", fuse_checksum, ETX);

	for (p = out_buffer; p < out_ptr; p++)
		file_checksum += (uint8_t)*p;

	sprintf(out_ptr, "%04X\r\n", file_checksum);

	fout = fopen(out_filename, "wb");
	if (!fout) {
		fprintf(stderr, "Cannot create: %s\n", out_filename);
		free(fuses);
		free(buffer);
		return 1;
	}
	fputs(out_buffer, fout);
	fclose(fout);

	printf("Wrote: %s\n", out_filename);

	free(fuses);
	free(buffer);
	return 0;
}
