#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <libfdt.h>
#include <sys/mman.h>

#include "bootimg.h"
#include "magiskboot.h"
#include "utils.h"
#include "logging.h"
#include "mincrypt/sha.h"
#include "mincrypt/sha256.h"

// Macros to determine header on-the-go
#define lheader(b, e, o) \
	((b)->flags & PXA_FLAG) ? \
	(((struct pxa_boot_img_hdr*) (b)->hdr)->e o) : \
	(((struct boot_img_hdr*) (b)->hdr)->e o)

#define header(b, e) (lheader(b, e,))

static void dump(void *buf, size_t size, const char *filename) {
	if (size == 0)
		return;
	int fd = creat(filename, 0644);
	xwrite(fd, buf, size);
	close(fd);
}

static size_t restore(const char *filename, int fd) {
	int ifd = xopen(filename, O_RDONLY);
	size_t size = lseek(ifd, 0, SEEK_END);
	lseek(ifd, 0, SEEK_SET);
	xsendfile(fd, ifd, NULL, size);
	close(ifd);
	return size;
}

static void restore_buf(int fd, const void *buf, size_t size) {
	xwrite(fd, buf, size);
}

static void print_hdr(const boot_img *boot) {
	fprintf(stderr, "KERNEL [%u]\n", header(boot, kernel_size));
	fprintf(stderr, "RAMDISK [%u]\n", header(boot, ramdisk_size));
	fprintf(stderr, "SECOND [%u]\n", header(boot, second_size));
	fprintf(stderr, "EXTRA [%u]\n", header(boot, extra_size));
	fprintf(stderr, "PAGESIZE [%u]\n", header(boot, page_size));

	if (!(boot->flags & PXA_FLAG)) {
		uint32_t os_version = ((boot_img_hdr*) boot->hdr)->os_version;
		if (os_version) {
			int a,b,c,y,m = 0;
			int version, patch_level;
			version = os_version >> 11;
			patch_level = os_version & 0x7ff;

			a = (version >> 14) & 0x7f;
			b = (version >> 7) & 0x7f;
			c = version & 0x7f;
			fprintf(stderr, "OS_VERSION [%d.%d.%d]\n", a, b, c);

			y = (patch_level >> 4) + 2000;
			m = patch_level & 0xf;
			fprintf(stderr, "PATCH_LEVEL [%d-%02d]\n", y, m);
		}
	}

	fprintf(stderr, "NAME [%s]\n", header(boot, name));
	fprintf(stderr, "CMDLINE [%s]\n", header(boot, cmdline));
	fprintf(stderr, "CHECKSUM [");
	for (int i = 0; i < ((boot->flags & SHA256_FLAG) ? SHA256_DIGEST_SIZE : SHA_DIGEST_SIZE); ++i)
		fprintf(stderr, "%02x", header(boot, id)[i]);
	fprintf(stderr, "]\n");
}

static void clean_boot(boot_img *boot) {
	munmap(boot->map_addr, boot->map_size);
	free(boot->hdr);
	free(boot->k_hdr);
	free(boot->r_hdr);
	free(boot->b_hdr);
	memset(boot, 0, sizeof(*boot));
}

#define CHROMEOS_RET       2
#define ELF32_RET          3
#define ELF64_RET          4
#define pos_align() pos = align(pos, header(boot, page_size))
int parse_img(const char *image, boot_img *boot) {
	memset(boot, 0, sizeof(*boot));
	mmap_ro(image, &boot->map_addr, &boot->map_size);

	// Parse image
	fprintf(stderr, "Parsing boot image: [%s]\n", image);
	for (void *head = boot->map_addr; head < boot->map_addr + boot->map_size; ++head) {
		size_t pos = 0;

		switch (check_fmt(head, boot->map_size)) {
		case CHROMEOS:
			// The caller should know it's chromeos, as it needs additional signing
			boot->flags |= CHROMEOS_FLAG;
			continue;
		case DHTB:
			boot->flags |= DHTB_FLAG;
			boot->flags |= SEANDROID_FLAG;
			fprintf(stderr, "DHTB_HDR\n");
			continue;
		case ELF32:
			exit(ELF32_RET);
		case ELF64:
			exit(ELF64_RET);
		case BLOB:
			boot->flags |= BLOB_FLAG;
			fprintf(stderr, "TEGRA_BLOB\n");
			boot->b_hdr = malloc(sizeof(blob_hdr));
			memcpy(boot->b_hdr, head, sizeof(blob_hdr));
			continue;
		case AOSP:
			// Read the header
			if (((boot_img_hdr*) head)->page_size >= 0x02000000) {
				boot->flags |= PXA_FLAG;
				fprintf(stderr, "PXA_BOOT_HDR\n");
				boot->hdr = malloc(sizeof(pxa_boot_img_hdr));
				memcpy(boot->hdr, head, sizeof(pxa_boot_img_hdr));
			} else if (memcmp(((boot_img_hdr*) head)->cmdline, NOOKHD_MAGIC, 12) == 0
					|| memcmp(((boot_img_hdr*) head)->cmdline, NOOKHD_NEW_MAGIC, 26) == 0) {
				boot->flags |= NOOKHD_FLAG;
				fprintf(stderr, "NOOKHD_GREEN_LOADER\n");
				head += NOOKHD_PRE_HEADER_SZ - 1;
				continue;
			} else if (memcmp(((boot_img_hdr*) head)->name, ACCLAIM_MAGIC, 10) == 0) {
				boot->flags |= ACCLAIM_FLAG;
				fprintf(stderr, "ACCLAIM_BAUWKSBOOT\n");
				head += ACCLAIM_PRE_HEADER_SZ - 1;
				continue;
			} else {
				boot->hdr = malloc(sizeof(boot_img_hdr));
				memcpy(boot->hdr, head, sizeof(boot_img_hdr));
			}
			pos += header(boot, page_size);

			for (int i = SHA_DIGEST_SIZE; i < SHA256_DIGEST_SIZE; ++i) {
				if (header(boot, id)[i]) {
					boot->flags |= SHA256_FLAG;
					break;
				}
			}

			print_hdr(boot);

			boot->kernel = head + pos;
			pos += header(boot, kernel_size);
			pos_align();

			boot->ramdisk = head + pos;
			pos += header(boot, ramdisk_size);
			pos_align();

			boot->second = head + pos;
			pos += header(boot, second_size);
			pos_align();

			boot->extra = head + pos;
			pos += header(boot, extra_size);
			pos_align();

			if (pos < boot->map_size) {
				boot->tail = head + pos;
				boot->tail_size = boot->map_size - (boot->tail - boot->map_addr);
			}

			// Check tail info, currently only for LG Bump and Samsung SEANDROIDENFORCE
			if (boot->tail_size >= 16 && memcmp(boot->tail, SEANDROID_MAGIC, 16) == 0) {
				boot->flags |= SEANDROID_FLAG;
			} else if (boot->tail_size >= 16 && memcmp(boot->tail, LG_BUMP_MAGIC, 16) == 0) {
				boot->flags |= LG_BUMP_FLAG;
			}

			// Search for dtb in kernel
			for (uint32_t i = 0; i < header(boot, kernel_size); ++i) {
				if (memcmp(boot->kernel + i, DTB_MAGIC, 4) == 0) {
					// Check that fdt_header.totalsize does not overflow kernel image size
					uint32_t dt_size = fdt32_to_cpu(*(uint32_t *)(boot->kernel + i + 4));
					if (dt_size > header(boot, kernel_size) - i) {
						fprintf(stderr, "Invalid DTB detection at 0x%x: size (%u) > remaining (%u)\n",
								i, dt_size, header(boot, kernel_size) - i);
						continue;
					}

					// Check that fdt_header.off_dt_struct does not overflow kernel image size
					uint32_t dt_struct_offset = fdt32_to_cpu(*(uint32_t *)(boot->kernel + i + 8));
					if (dt_struct_offset > header(boot, kernel_size) - i) {
						fprintf(stderr, "Invalid DTB detection at 0x%x: struct offset (%u) > remaining (%u)\n",
								i, dt_struct_offset, header(boot, kernel_size) - i);
						continue;
					}

					// Check that fdt_node_header.tag of first node is FDT_BEGIN_NODE
					uint32_t dt_begin_node = fdt32_to_cpu(*(uint32_t *)(boot->kernel + i + dt_struct_offset));
					if (dt_begin_node != FDT_BEGIN_NODE) {
						fprintf(stderr, "Invalid DTB detection at 0x%x: header tag of first node != FDT_BEGIN_NODE\n", i);
						continue;
					}

					boot->dtb = boot->kernel + i;
					boot->dt_size = header(boot, kernel_size) - i;
					lheader(boot, kernel_size, = i);
					fprintf(stderr, "DTB [%u]\n", boot->dt_size);
					break;
				}
			}

			boot->k_fmt = check_fmt(boot->kernel, header(boot, kernel_size));
			boot->r_fmt = check_fmt(boot->ramdisk, header(boot, ramdisk_size));

			// Check MTK
			if (boot->k_fmt == MTK) {
				fprintf(stderr, "MTK_KERNEL_HDR\n");
				boot->flags |= MTK_KERNEL;
				boot->k_hdr = malloc(sizeof(mtk_hdr));
				memcpy(boot->k_hdr, boot->kernel, sizeof(mtk_hdr));
				fprintf(stderr, "KERNEL [%u]\n", boot->k_hdr->size);
				fprintf(stderr, "NAME [%s]\n", boot->k_hdr->name);
				boot->kernel += 512;
				lheader(boot, kernel_size, -= 512);
				boot->k_fmt = check_fmt(boot->kernel, header(boot, kernel_size));
			}
			if (boot->r_fmt == MTK) {
				fprintf(stderr, "MTK_RAMDISK_HDR\n");
				boot->flags |= MTK_RAMDISK;
				boot->r_hdr = malloc(sizeof(mtk_hdr));
				memcpy(boot->r_hdr, boot->ramdisk, sizeof(mtk_hdr));
				fprintf(stderr, "RAMDISK [%u]\n", boot->r_hdr->size);
				fprintf(stderr, "NAME [%s]\n", boot->r_hdr->name);
				boot->ramdisk += 512;
				lheader(boot, ramdisk_size, -= 512);
				boot->r_fmt = check_fmt(boot->ramdisk, header(boot, ramdisk_size));
			}

			char fmt[16];
			get_fmt_name(boot->k_fmt, fmt);
			fprintf(stderr, "KERNEL_FMT [%s]\n", fmt);
			get_fmt_name(boot->r_fmt, fmt);
			fprintf(stderr, "RAMDISK_FMT [%s]\n", fmt);

			return boot->flags & CHROMEOS_FLAG ? CHROMEOS_RET : 0;
		default:
			continue;
		}
	}
	LOGE("No boot image magic found!\n");
	exit(1);
}

int unpack(const char *image) {
	boot_img boot;
	int ret = parse_img(image, &boot);
	int fd;

	// Dump kernel
	if (COMPRESSED(boot.k_fmt)) {
		fd = creat(KERNEL_FILE, 0644);
		decomp(boot.k_fmt, fd, boot.kernel, header(&boot, kernel_size));
		close(fd);
	} else {
		dump(boot.kernel, header(&boot, kernel_size), KERNEL_FILE);
	}

	// Dump dtb
	dump(boot.dtb, boot.dt_size, DTB_FILE);

	// Dump ramdisk
	if (COMPRESSED(boot.r_fmt)) {
		fd = creat(RAMDISK_FILE, 0644);
		decomp(boot.r_fmt, fd, boot.ramdisk, header(&boot, ramdisk_size));
		close(fd);
	} else {
		dump(boot.ramdisk, header(&boot, ramdisk_size), RAMDISK_FILE);
	}

	// Dump second
	dump(boot.second, header(&boot, second_size), SECOND_FILE);

	// Dump extra
	dump(boot.extra, header(&boot, extra_size), EXTRA_FILE);

	clean_boot(&boot);
	return ret;
}

#define file_align() write_zero(fd, align_off(lseek(fd, 0, SEEK_CUR) - header_off, header(&boot, page_size)))
void repack(const char* orig_image, const char* out_image) {
	boot_img boot;

	off_t header_off, kernel_off, ramdisk_off, second_off, extra_off;

	// Parse original image
	parse_img(orig_image, &boot);

	// Reset all sizes
	lheader(&boot, kernel_size, = 0);
	lheader(&boot, ramdisk_size, = 0);
	lheader(&boot, second_size, = 0);
	lheader(&boot, extra_size, = 0);
	boot.dt_size = 0;

	fprintf(stderr, "Repack to boot image: [%s]\n", out_image);

	// Create new image
	int fd = creat(out_image, 0644);

	if (boot.flags & DHTB_FLAG) {
		// Skip DHTB header
		write_zero(fd, 512);
	} else if (boot.flags & BLOB_FLAG) {
		// Skip blob header
		write_zero(fd, sizeof(blob_hdr));
	} else if (boot.flags & NOOKHD_FLAG) {
		restore_buf(fd, boot.map_addr, NOOKHD_PRE_HEADER_SZ);
	} else if (boot.flags & ACCLAIM_FLAG) {
		restore_buf(fd, boot.map_addr, ACCLAIM_PRE_HEADER_SZ);
	}

	// Skip a page for header
	header_off = lseek(fd, 0, SEEK_CUR);
	write_zero(fd, header(&boot, page_size));

	// kernel
	kernel_off = lseek(fd, 0, SEEK_CUR);
	if (boot.flags & MTK_KERNEL) {
		// Skip MTK header
		write_zero(fd, 512);
	}
	if (access(KERNEL_FILE, R_OK) == 0) {
		if (COMPRESSED(boot.k_fmt)) {
			size_t raw_size;
			void *kernel_raw;
			mmap_ro(KERNEL_FILE, &kernel_raw, &raw_size);
			lheader(&boot, kernel_size, = comp(boot.k_fmt, fd, kernel_raw, raw_size));
			munmap(kernel_raw, raw_size);
		} else {
			lheader(&boot, kernel_size, = restore(KERNEL_FILE, fd));
		}
	}

	// dtb
	if (access(DTB_FILE, R_OK) == 0) {
		lheader(&boot, kernel_size, += restore(DTB_FILE, fd));
	}
	file_align();

	// ramdisk
	ramdisk_off = lseek(fd, 0, SEEK_CUR);
	if (boot.flags & MTK_RAMDISK) {
		// Skip MTK header
		write_zero(fd, 512);
	}
	if (access(RAMDISK_FILE, R_OK) == 0) {
		if (COMPRESSED(boot.r_fmt)) {
			size_t cpio_size;
			void *cpio;
			mmap_ro(RAMDISK_FILE, &cpio, &cpio_size);
			lheader(&boot, ramdisk_size, = comp(boot.r_fmt, fd, cpio, cpio_size));
			munmap(cpio, cpio_size);
		} else {
			lheader(&boot, ramdisk_size, = restore(RAMDISK_FILE, fd));
		}
		file_align();
	}

	// second
	second_off = lseek(fd, 0, SEEK_CUR);
	if (access(SECOND_FILE, R_OK) == 0) {
		lheader(&boot, second_size, = restore(SECOND_FILE, fd));
		file_align();
	}

	// extra
	extra_off = lseek(fd, 0, SEEK_CUR);
	if (access(EXTRA_FILE, R_OK) == 0) {
		lheader(&boot, extra_size, = restore(EXTRA_FILE, fd));
		file_align();
	}

	// Append tail info
	if (boot.flags & SEANDROID_FLAG) {
		restore_buf(fd, SEANDROID_MAGIC "\xFF\xFF\xFF\xFF", 20);
	}
	if (boot.flags & LG_BUMP_FLAG) {
		restore_buf(fd, LG_BUMP_MAGIC, 16);
	}

	close(fd);

	// Map output image as rw
	munmap(boot.map_addr, boot.map_size);
	mmap_rw(out_image, &boot.map_addr, &boot.map_size);

	// MTK headers
	if (boot.flags & MTK_KERNEL) {
		boot.k_hdr->size = header(&boot, kernel_size);
		lheader(&boot, kernel_size, += 512);
		memcpy(boot.map_addr + kernel_off, boot.k_hdr, sizeof(mtk_hdr));
	}
	if (boot.flags & MTK_RAMDISK) {
		boot.r_hdr->size = header(&boot, ramdisk_size);
		lheader(&boot, ramdisk_size, += 512);
		memcpy(boot.map_addr + ramdisk_off, boot.r_hdr, sizeof(mtk_hdr));
	}

	// Update checksum
	HASH_CTX ctx;
	(boot.flags & SHA256_FLAG) ? SHA256_init(&ctx) : SHA_init(&ctx);
	uint32_t size = header(&boot, kernel_size);
	HASH_update(&ctx, boot.map_addr + kernel_off, size);
	HASH_update(&ctx, &size, sizeof(size));
	size = header(&boot, ramdisk_size);
	HASH_update(&ctx, boot.map_addr + ramdisk_off, size);
	HASH_update(&ctx, &size, sizeof(size));
	size = header(&boot, second_size);
	HASH_update(&ctx, boot.map_addr + second_off, size);
	HASH_update(&ctx, &size, sizeof(size));
	size = header(&boot, extra_size);
	if (size) {
		HASH_update(&ctx, boot.map_addr + extra_off, size);
		HASH_update(&ctx, &size, sizeof(size));
	}
	memset(header(&boot, id), 0, 32);
	memcpy(header(&boot, id), HASH_final(&ctx),
		   (boot.flags & SHA256_FLAG) ? SHA256_DIGEST_SIZE : SHA_DIGEST_SIZE);

	// Print new image info
	print_hdr(&boot);

	// Main header
	memcpy(boot.map_addr + header_off, boot.hdr,
		   (boot.flags & PXA_FLAG) ? sizeof(pxa_boot_img_hdr) : sizeof(boot_img_hdr));

	if (boot.flags & DHTB_FLAG) {
		// DHTB header
		dhtb_hdr *hdr = boot.map_addr;
		memcpy(hdr, DHTB_MAGIC, 8);
		hdr->size = boot.map_size - 512;
		SHA256_hash(boot.map_addr + 512, hdr->size, (uint8_t*) hdr->checksum);
	} else if (boot.flags & BLOB_FLAG) {
		// Blob headers
		boot.b_hdr->size = boot.map_size - sizeof(blob_hdr);
		memcpy(boot.map_addr, boot.b_hdr, sizeof(blob_hdr));
	}

	clean_boot(&boot);
}
