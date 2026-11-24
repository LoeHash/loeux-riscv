/*
 * lcc.c
 *
 * A very small self-hostable MiniC -> RV64 ELF64 compiler.
 *

 * The compiler itself is intended to run as a user process of loeux.
 * It therefore uses only the project's user headers and these runtime
 * interfaces:
 *
 *     open / read / write / close
 *     malloc / calloc / realloc / free
 *
 * It does NOT use fork/exec, as, ld, system(), popen(), mmap(), or any
 * host-specific process facilities.
 *
 * The final executable is linked completely in memory and written out
 * directly as one ELF64 ET_EXEC file.
 */

#include "utype.h"
#include "malloc.h"
#include "stdio.h"
#include "ufile.h"
#include "ulib.h"
#include "umath.h"
#include "ustring.h"

#define LIB_DIR "/lib"
#define INCLUDE_DIR "/lib"
#define CRT0_OBJ "/lib/crt0.o"
#define ULIB_OBJ "/lib/ulib.o"
#define MALLOC_OBJ "/lib/malloc.o"
#define STDIO_OBJ "/lib/stdio.o"
#define DEFAULT_LD "/bin/ld"
#define LINKER_SCRIPT "/lib/loeuxcc.ld"

#define TEXT_BASE 0x1000ULL
#define PAGE_SIZE 0x1000ULL
#define STDERR_FD 2
#define STDOUT_FD 1
#define MAX_ARGS 8

static unsigned long long g_link_data_base;

static const char* g_include_dir = INCLUDE_DIR;
static const char* g_lib_dir = LIB_DIR;

/* We only fall back to the normal POSIX values when the target header
 * did not provide them. The target's definitions, when present, win. */
#ifndef O_RDONLY
#define O_RDONLY 0
#endif
#ifndef O_WRONLY
#define O_WRONLY 1
#endif
#ifndef O_RDWR
#define O_RDWR 2
#endif
#ifndef O_CREAT
#define O_CREAT 0100
#endif
#ifndef O_TRUNC
#define O_TRUNC 01000
#endif

static void* mc_memcpy(void* dst, const void* src, unsigned long n);
static void* mc_memset(void* dst, int c, unsigned long n);

static void* xmalloc(unsigned long n)
{
	void* p = malloc(n ? n : 1);
	if (!p) {
		return 0;
	}
	return p;
}

static void* xcalloc(unsigned long n, unsigned long s)
{
	void* p;
	if (n && s > ~0UL / n) {
		return 0;
	}
	p = calloc(n, s);
	return p;
}

static char* xstrdup_n(const char* s, unsigned long n)
{
	char* p = (char*)xmalloc(n + 1);
	if (!p)
		return 0;
	if (n)
		mc_memcpy(p, s, n);
	p[n] = 0;
	return p;
}

static unsigned long u_strlen(const char* s)
{
	unsigned long n = 0;
	while (s[n])
		++n;
	return n;
}

static void* mc_memcpy(void* dst, const void* src, unsigned long n)
{
	char* d = (char*)dst;
	const char* s = (const char*)src;
	unsigned long i;
	for (i = 0; i < n; ++i)
		d[i] = s[i];
	__asm__ volatile("" : : "r"(d), "r"(s) : "memory");
	return dst;
}

static void* mc_memset(void* dst, int c, unsigned long n)
{
	char* d = (char*)dst;
	unsigned long i;
	for (i = 0; i < n; ++i)
		d[i] = (char)c;
	__asm__ volatile("" : : "r"(d) : "memory");
	return dst;
}

static int mc_is_alpha(int c)
{
	return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

static int mc_is_digit(int c)
{
	return c >= '0' && c <= '9';
}
static int mc_is_xdigit(int c)
{
	return mc_is_digit(c) || (c >= 'a' && c <= 'f') ||
	       (c >= 'A' && c <= 'F');
}
static int mc_is_alnum(int c)
{
	return mc_is_alpha(c) || mc_is_digit(c);
}
static int mc_char_in(const char* set, int c)
{
	while (*set) {
		if (*set++ == c)
			return 1;
	}
	return 0;
}

static void mc_copy(char* dst, const char* src)
{
	while ((*dst++ = *src++) != 0) {
	}
}

static void mc_cat(char* dst, const char* src)
{
	while (*dst)
		++dst;
	mc_copy(dst, src);
}

static int u_streq(const char* a, const char* b)
{
	unsigned long i = 0;
	while (a[i] && b[i] && a[i] == b[i])
		++i;
	return a[i] == b[i];
}

static int u_strncmp_n(const char* a, const char* b, unsigned long n)
{
	unsigned long i;
	for (i = 0; i < n; ++i) {
		unsigned char ca = (unsigned char)a[i];
		unsigned char cb = (unsigned char)b[i];
		if (ca != cb)
			return (int)ca - (int)cb;
		if (!ca)
			return 0;
	}
	return 0;
}

static unsigned long align_up(unsigned long x, unsigned long a)
{
	return (x + a - 1) & ~(a - 1);
}

static unsigned int rd32(const unsigned char* p)
{
	return (unsigned int)p[0] | ((unsigned int)p[1] << 8) |
	       ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
}

static void wr16(unsigned char* p, unsigned short x)
{
	p[0] = (unsigned char)x;
	p[1] = (unsigned char)(x >> 8);
}

static void wr32(unsigned char* p, unsigned int x)
{
	p[0] = (unsigned char)x;
	p[1] = (unsigned char)(x >> 8);
	p[2] = (unsigned char)(x >> 16);
	p[3] = (unsigned char)(x >> 24);
}

static void wr64(unsigned char* p, unsigned long long x)
{
	int i;
	for (i = 0; i < 8; ++i)
		p[i] = (unsigned char)(x >> (8 * i));
}

static void put_u64_dec(char* buf, unsigned long long v)
{
	char tmp[32];
	int n = 0;
	int i;
	if (v == 0) {
		buf[0] = '0';
		buf[1] = 0;
		return;
	}
	while (v) {
		tmp[n++] = (char)('0' + v % 10);
		v /= 10;
	}
	for (i = 0; i < n; ++i)
		buf[i] = tmp[n - i - 1];
	buf[n] = 0;
}

static void put_hex8(char* buf, unsigned char v)
{
	static const char hex[] = "0123456789abcdef";
	buf[0] = hex[(v >> 4) & 0xf];
	buf[1] = hex[v & 0xf];
	buf[2] = 0;
}

static void die_plain(const char* msg)
{
	write(STDERR_FD, (unsigned char*)msg, u_strlen(msg));
	write(STDERR_FD, (unsigned char*)"\n", 1);
}

static void dief(const char* file, int line, int col, const char* msg)
{
	char a[32], b[32];
	write(STDERR_FD, (unsigned char*)file, u_strlen(file));
	write(STDERR_FD, (unsigned char*)"::", 1);
	put_u64_dec(a, (unsigned long long)line);
	write(STDERR_FD, a, u_strlen(a));
	write(STDERR_FD, (unsigned char*)"::", 1);
	put_u64_dec(b, (unsigned long long)col);
	write(STDERR_FD, b, u_strlen(b));
	write(STDERR_FD, (unsigned char*)": error: ", 9);
	write(STDERR_FD, (unsigned char*)msg, u_strlen(msg));
	write(STDERR_FD, (unsigned char*)"\n", 1);
}

static int
file_read_all(const char* path, unsigned char** out, unsigned long* out_n)
{
	int fd;
	unsigned long cap = 4096;
	unsigned long n = 0;
	unsigned char* buf;

	fd = open(path, O_RDONLY);
	if (fd < 0)
		return -1;

	buf = (unsigned char*)xmalloc(cap);
	if (!buf) {
		close(fd);
		return -1;
	}

	for (;;) {
		long long r = read(fd, buf + n, cap - n);
		if (r < 0) {
			free(buf);
			close(fd);
			return -1;
		}
		if (r == 0)
			break;
		n += (unsigned long)r;
		if (n == cap) {
			unsigned long newcap = cap * 2;
			unsigned char* nb =
			    (unsigned char*)realloc(buf, newcap);
			if (!nb) {
				free(buf);
				close(fd);
				return -1;
			}
			buf = nb;
			cap = newcap;
		}
	}

	close(fd);
	*out = buf;
	*out_n = n;
	return 0;
}

static int
file_write_all(const char* path, const unsigned char* buf, unsigned long n)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC);
	unsigned long off = 0;
	if (fd < 0)
		return -1;
	while (off < n) {
		long long w = write(fd, buf + off, n - off);
		if (w <= 0) {
			close(fd);
			return -1;
		}
		off += (unsigned long)w;
	}
	close(fd);
	return 0;
}

struct ByteVec {
	unsigned char* p;
	unsigned long n;
	unsigned long cap;
};

static void bv_init(struct ByteVec* v)
{
	v->p = 0;
	v->n = 0;
	v->cap = 0;
}

static int bv_reserve(struct ByteVec* v, unsigned long need)
{
	unsigned long cap;
	unsigned char* p;
	if (need <= v->cap)
		return 0;
	cap = v->cap ? v->cap : 256;
	while (cap < need) {
		if (cap > ~0UL / 2)
			return -1;
		cap *= 2;
	}
	p = (unsigned char*)realloc(v->p, cap);
	if (!p)
		return -1;
	v->p = p;
	v->cap = cap;
	return 0;
}

static int bv_resize(struct ByteVec* v, unsigned long n)
{
	if (bv_reserve(v, n) < 0)
		return -1;
	if (n > v->n)
		mc_memset(v->p + v->n, 0, n - v->n);
	v->n = n;
	return 0;
}

static int bv_append(struct ByteVec* v, const void* data, unsigned long n)
{
	if (bv_resize(v, v->n + n) < 0)
		return -1;
	if (n)
		mc_memcpy(v->p + v->n - n, data, n);
	return 0;
}

static int bv_u32(struct ByteVec* v, unsigned int x)
{
	unsigned char b[4];
	wr32(b, x);
	return bv_append(v, b, 4);
}

static int bv_u64(struct ByteVec* v, unsigned long long x)
{
	unsigned char b[8];
	wr64(b, x);
	return bv_append(v, b, 8);
}

static int bv_align(struct ByteVec* v, unsigned long a)
{
	return bv_resize(v, align_up(v->n, a));
}

#define EI_NIDENT 16
#define ELFCLASS64 2
#define ELFDATA2LSB 1
#define ET_REL 1
#define ET_EXEC 2
#define EM_RISCV 243
#define PT_LOAD 1
#define PF_X 1
#define PF_W 2
#define PF_R 4
#define SHT_NULL 0
#define SHT_PROGBITS 1
#define SHT_SYMTAB 2
#define SHT_STRTAB 3
#define SHT_RELA 4
#define SHT_NOBITS 8
#define SHF_WRITE 1
#define SHF_ALLOC 2
#define SHF_EXECINSTR 4
#define SHN_UNDEF 0
#define SHN_ABS 0xfff1
#define STB_LOCAL 0
#define STB_GLOBAL 1
#define STT_NOTYPE 0
#define STT_OBJECT 1
#define STT_FUNC 2
#define ST_INFO(b, t) (((b) << 4) | ((t) & 0xf))

#define R_RISCV_NONE 0
#define R_RISCV_32 1
#define R_RISCV_64 2
#define R_RISCV_BRANCH 16
#define R_RISCV_JAL 17
#define R_RISCV_CALL 18
#define R_RISCV_CALL_PLT 19
#define R_RISCV_RVC_BRANCH 44
#define R_RISCV_RVC_JUMP 45
#define R_RISCV_PCREL_HI20 23
#define R_RISCV_PCREL_LO12_I 24
#define R_RISCV_PCREL_LO12_S 25
#define R_RISCV_HI20 26
#define R_RISCV_LO12_I 27
#define R_RISCV_LO12_S 28
#define R_RISCV_ALIGN 43
#define R_RISCV_RELAX 51

struct Elf64_Ehdr {
	unsigned char e_ident[16];
	unsigned short e_type;
	unsigned short e_machine;
	unsigned int e_version;
	unsigned long long e_entry;
	unsigned long long e_phoff;
	unsigned long long e_shoff;
	unsigned int e_flags;
	unsigned short e_ehsize;
	unsigned short e_phentsize;
	unsigned short e_phnum;
	unsigned short e_shentsize;
	unsigned short e_shnum;
	unsigned short e_shstrndx;
};

struct Elf64_Phdr {
	unsigned int p_type;
	unsigned int p_flags;
	unsigned long long p_offset;
	unsigned long long p_vaddr;
	unsigned long long p_paddr;
	unsigned long long p_filesz;
	unsigned long long p_memsz;
	unsigned long long p_align;
};

struct Elf64_Shdr {
	unsigned int sh_name;
	unsigned int sh_type;
	unsigned long long sh_flags;
	unsigned long long sh_addr;
	unsigned long long sh_offset;
	unsigned long long sh_size;
	unsigned int sh_link;
	unsigned int sh_info;
	unsigned long long sh_addralign;
	unsigned long long sh_entsize;
};

struct Elf64_Sym {
	unsigned int st_name;
	unsigned char st_info;
	unsigned char st_other;
	unsigned short st_shndx;
	unsigned long long st_value;
	unsigned long long st_size;
};

struct Elf64_Rela {
	unsigned long long r_offset;
	unsigned long long r_info;
	long long r_addend;
};

static unsigned int r_sym(unsigned long long x)
{
	return (unsigned int)(x >> 32);
}
static unsigned int r_type(unsigned long long x)
{
	return (unsigned int)x;
}
static unsigned long long r_info(unsigned int s, unsigned int t)
{
	return ((unsigned long long)s << 32) | t;
}

/* ---- 宏表 ---- */
#define MAX_MACROS 512
struct Macro {
	char name[64];
	char value[512];
	int has_value; /* 0 = defined but empty, 1 = has value */
};
static struct Macro g_macros[MAX_MACROS];
static int g_nmacros = 0;

static int macro_find(const char* name)
{
	int i;
	for (i = 0; i < g_nmacros; ++i)
		if (u_streq(name, g_macros[i].name))
			return i;
	return -1;
}

static void macro_add(const char* name, const char* value)
{
	int idx;
	if (g_nmacros >= MAX_MACROS)
		return;
	idx = macro_find(name);
	if (idx < 0)
		idx = g_nmacros++;
	else {
		/* 重定义：直接覆盖 */
	}
	{
		unsigned long nl = u_strlen(name);
		if (nl > 63)
			nl = 63;
		mc_memcpy(g_macros[idx].name, name, nl);
		g_macros[idx].name[nl] = 0;
	}
	if (value) {
		unsigned long vl = u_strlen(value);
		if (vl > 511)
			vl = 511;
		mc_memcpy(g_macros[idx].value, value, vl);
		g_macros[idx].value[vl] = 0;
		g_macros[idx].has_value = 1;
	} else {
		g_macros[idx].value[0] = 0;
		g_macros[idx].has_value = 0;
	}
}

static void macro_remove(const char* name)
{
	int idx = macro_find(name);
	if (idx >= 0) {
		g_macros[idx] = g_macros[--g_nmacros];
	}
}

/* ---- 条件编译栈 ---- */
#define MAX_COND_STACK 32
static int g_cond_stack[MAX_COND_STACK];
static int g_cond_sp = 0;
/* g_cond_stack[i] = 1 表示当前层 active（输出），0 表示跳过 */

static int cond_active(void)
{
	int i;
	for (i = 0; i < g_cond_sp; ++i)
		if (!g_cond_stack[i])
			return 0;
	return 1;
}

/* 判断字符是否为标识符字符 */
static int is_ident_start(int c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
static int is_ident_char(int c)
{
	return is_ident_start(c) || (c >= '0' && c <= '9');
}

/* 对一行做宏替换（object-like only）。
 * 返回新分配的字符串，调用者负责 free。 */
static char* macro_substitute(const char* line, unsigned long len)
{
	struct ByteVec out;
	unsigned long i = 0;
	bv_init(&out);
	while (i < len) {
		unsigned long start = i;
		/* 跳过字符串字面量 */
		if (line[i] == '"') {
			bv_append(&out, line + i, 1);
			++i;
			while (i < len && line[i] != '"') {
				if (line[i] == '\\' && i + 1 < len) {
					bv_append(&out, line + i, 2);
					i += 2;
				} else {
					bv_append(&out, line + i, 1);
					++i;
				}
			}
			if (i < len) {
				bv_append(&out, line + i, 1);
				++i;
			}
			continue;
		}
		/* 跳过字符字面量 */
		if (line[i] == '\'') {
			bv_append(&out, line + i, 1);
			++i;
			while (i < len && line[i] != '\'') {
				if (line[i] == '\\' && i + 1 < len) {
					bv_append(&out, line + i, 2);
					i += 2;
				} else {
					bv_append(&out, line + i, 1);
					++i;
				}
			}
			if (i < len) {
				bv_append(&out, line + i, 1);
				++i;
			}
			continue;
		}
		/* 识别标识符 */
		if (is_ident_start((unsigned char)line[i])) {
			start = i;
			++i;
			while (i < len && is_ident_char((unsigned char)line[i]))
				++i;
			{
				unsigned long namelen = i - start;
				char namebuf[64];
				int midx;
				if (namelen >= 64)
					namelen = 63;
				mc_memcpy(namebuf, line + start, namelen);
				namebuf[namelen] = 0;
				/* 不替换后面紧跟 ( 的标识符（function-like
				 * macro） */
				if (i < len && line[i] == '(') {
					/* function-like macro 调用 — 原样输出
					 */
					bv_append(
					    &out, line + start, i - start);
					continue;
				}
				midx = macro_find(namebuf);
				if (midx >= 0 && g_macros[midx].has_value) {
					/* 递归替换宏值（防止无限递归：简单深度限制）
					 */
					bv_append(
					    &out,
					    g_macros[midx].value,
					    u_strlen(g_macros[midx].value));
				} else {
					bv_append(
					    &out, line + start, i - start);
				}
			}
			continue;
		}
		/* 普通字符 */
		if (bv_append(&out, line + i, 1) < 0)
			die_plain("lcc: out of memory");
		++i;
	}
	if (bv_append(&out, "", 1) < 0)
		die_plain("lcc: out of memory");
	return (char*)out.p;
}

static char* join_path(const char* a, const char* b)
{
	unsigned long na = u_strlen(a);
	unsigned long nb = u_strlen(b);
	int slash = na && a[na - 1] != '/';
	char* r = (char*)xmalloc(na + nb + slash + 1);
	if (!r)
		return 0;
	mc_memcpy(r, a, na);
	if (slash)
		r[na++] = '/';
	mc_memcpy(r + na, b, nb);
	r[na + nb] = 0;
	return r;
}

static char* lib_path(const char* name)
{
	return join_path(g_lib_dir, name);
}

/* Forward declaration. */
static char* expand_file(const char* path, int depth);

static char* expand_source_text(const char* path,
				const unsigned char* src,
				unsigned long n,
				int depth)
{
	(void)path;
	struct ByteVec out;
	unsigned long i = 0;
	int skip_c_decl = 0;
	int skip_brace_depth = 0;
	int skip_seen_brace = 0;
	bv_init(&out);

	while (i < n) {
		unsigned long line_start = i;
		unsigned long line_end = i;
		while (line_end < n && src[line_end] != '\n')
			++line_end;

		/* Recognize only #include. All other lines are passed through
		 * literally. */
		unsigned long p = line_start;
		while (p < line_end &&
		       (src[p] == ' ' || src[p] == '\t' || src[p] == '\r'))
			++p;

		/* Header-only declarations that MiniC does not need
		 * semantically: typedefs and complete struct definitions. Keep
		 * a few common ABI typedef names as built-in type names in the
		 * parser instead. */
		if (cond_active() && skip_c_decl) {
			unsigned long z;
			for (z = line_start; z < line_end; ++z) {
				if (src[z] == '{') {
					skip_seen_brace = 1;
					++skip_brace_depth;
				} else if (src[z] == '}') {
					if (skip_brace_depth)
						--skip_brace_depth;
				}
			}
			if (!skip_brace_depth && skip_seen_brace)
				skip_c_decl = 0;
			if (line_end < n && src[line_end] == '\n')
				bv_append(&out, "\n", 1);
			i = line_end;
			if (i < n && src[i] == '\n')
				++i;
			continue;
		}

		if (cond_active() && p + 7 <= line_end &&
		    !u_strncmp_n((const char*)(src + p), "typedef", 7) &&
		    (p + 7 == line_end || src[p + 7] == ' ' ||
		     src[p + 7] == '\t' || src[p + 7] == '\r')) {
			unsigned long z;
			int depth2 = 0;
			for (z = p; z < line_end; ++z) {
				if (src[z] == '{')
					++depth2;
				else if (src[z] == '}') {
					if (depth2)
						--depth2;
				}
			}
			if (depth2) {
				skip_c_decl = 1;
				skip_brace_depth = depth2;
				skip_seen_brace = 1;
				if (line_end < n && src[line_end] == '\n')
					bv_append(&out, "\n", 1);
				i = line_end;
				if (i < n && src[i] == '\n')
					++i;
				continue;
			}
			/* simple typedef (no braces): fall through to
			 * default output so the parser sees the typedef */
		}

		if (cond_active() && p + 6 <= line_end &&
		    !u_strncmp_n((const char*)(src + p), "struct", 6) &&
		    (p + 6 == line_end || src[p + 6] == ' ' ||
		     src[p + 6] == '\t' || src[p + 6] == '\r')) {
			unsigned long z;
			int has_brace = 0;
			int depth2 = 0;
			for (z = p; z < line_end; ++z) {
				if (src[z] == '{') {
					has_brace = 1;
					++depth2;
				} else if (src[z] == '}') {
					if (depth2)
						--depth2;
				}
			}
			if (has_brace) {
				if (depth2) {
					skip_c_decl = 1;
					skip_brace_depth = depth2;
					skip_seen_brace = 1;
					if (line_end < n &&
					    src[line_end] == '\n')
						bv_append(&out, "\n", 1);
					i = line_end;
					if (i < n && src[i] == '\n')
						++i;
					continue;
				}
				/* balanced braces on same line: fall
				 * through to default output */
			}
			/* Multi-line struct definition: "struct name" with
			 * no brace and no semicolon on this line — the
			 * opening brace is on a following line. Enter skip
			 * mode and wait for the brace. */
			{
				int has_semi = 0;
				int has_other = 0;
				for (z = p + 6; z < line_end; ++z) {
					if (src[z] == ';')
						has_semi = 1;
					else if (src[z] != ' ' &&
						 src[z] != '\t' &&
						 src[z] != '\r')
						has_other = 1;
				}
				if (!has_semi && !has_brace) {
					skip_c_decl = 1;
					skip_brace_depth = 0;
					skip_seen_brace = 0;
					if (line_end < n &&
					    src[line_end] == '\n')
						bv_append(&out, "\n", 1);
					i = line_end;
					if (i < n && src[i] == '\n')
						++i;
					continue;
				}
			}
		}

		/* static inline function definitions — skip entire body.
		 * lcc cannot parse the complex C constructs inside these
		 * functions (for-init declarations, casts, pointer arithmetic,
		 * static locals, etc.). The .o files already provide the code
		 * for functions that are actually used. */
		if (cond_active() && !skip_c_decl && p + 6 <= line_end &&
		    !u_strncmp_n((const char*)(src + p), "static", 6) &&
		    (p + 6 == line_end || src[p + 6] == ' ' ||
		     src[p + 6] == '\t' || src[p + 6] == '\r')) {
			unsigned long q2 = p + 6;
			while (q2 < line_end &&
			       (src[q2] == ' ' || src[q2] == '\t'))
				++q2;
			if (q2 + 6 <= line_end &&
			    !u_strncmp_n(
				(const char*)(src + q2), "inline", 6) &&
			    (q2 + 6 == line_end || src[q2 + 6] == ' ' ||
			     src[q2 + 6] == '\t' || src[q2 + 6] == '\r')) {
				unsigned long z;
				int depth2 = 0;
				for (z = p; z < line_end; ++z) {
					if (src[z] == '{')
						++depth2;
					else if (src[z] == '}') {
						if (depth2)
							--depth2;
					}
				}
				skip_c_decl = 1;
				skip_brace_depth = depth2;
				skip_seen_brace = (depth2 > 0) ? 1 : 0;
				if (line_end < n && src[line_end] == '\n')
					bv_append(&out, "\n", 1);
				i = line_end;
				if (i < n && src[i] == '\n')
					++i;
				continue;
			}
		}

		/* ---- # 预处理指令 ---- */
		if (p < line_end && src[p] == '#') {
			unsigned long q = p + 1;
			unsigned long d_start;
			while (q < line_end &&
			       (src[q] == ' ' || src[q] == '\t'))
				++q;
			d_start = q;
			while (q < line_end &&
			       is_ident_start((unsigned char)src[q]))
				++q;
			unsigned long d_len = q - d_start;
			char directive[16];
			if (d_len >= 16)
				d_len = 15;
			mc_memcpy(directive, src + d_start, d_len);
			directive[d_len] = 0;

			/* 条件编译指令：始终处理（即使在 inactive 块内） */
			if (u_streq(directive, "ifndef")) {
				while (q < line_end &&
				       (src[q] == ' ' || src[q] == '\t'))
					++q;
				unsigned long nm_start = q;
				while (q < line_end &&
				       is_ident_char((unsigned char)src[q]))
					++q;
				if (q > nm_start &&
				    g_cond_sp < MAX_COND_STACK) {
					char nm[64];
					unsigned long nl = q - nm_start;
					if (nl > 63)
						nl = 63;
					mc_memcpy(nm, src + nm_start, nl);
					nm[nl] = 0;
					g_cond_stack[g_cond_sp++] =
					    (macro_find(nm) < 0) ? 1 : 0;
				} else
					g_cond_stack[g_cond_sp++] = 0;
				/* 输出空行保持行号 */
				bv_append(&out, "\n", 1);
				i = line_end;
				if (i < n && src[i] == '\n')
					++i;
				continue;
			}
			if (u_streq(directive, "ifdef")) {
				while (q < line_end &&
				       (src[q] == ' ' || src[q] == '\t'))
					++q;
				unsigned long nm_start = q;
				while (q < line_end &&
				       is_ident_char((unsigned char)src[q]))
					++q;
				if (q > nm_start &&
				    g_cond_sp < MAX_COND_STACK) {
					char nm[64];
					unsigned long nl = q - nm_start;
					if (nl > 63)
						nl = 63;
					mc_memcpy(nm, src + nm_start, nl);
					nm[nl] = 0;
					g_cond_stack[g_cond_sp++] =
					    (macro_find(nm) >= 0) ? 1 : 0;
				} else
					g_cond_stack[g_cond_sp++] = 0;
				bv_append(&out, "\n", 1);
				i = line_end;
				if (i < n && src[i] == '\n')
					++i;
				continue;
			}
			if (u_streq(directive, "else")) {
				if (g_cond_sp > 0)
					g_cond_stack[g_cond_sp - 1] =
					    !g_cond_stack[g_cond_sp - 1];
				bv_append(&out, "\n", 1);
				i = line_end;
				if (i < n && src[i] == '\n')
					++i;
				continue;
			}
			if (u_streq(directive, "endif")) {
				if (g_cond_sp > 0)
					--g_cond_sp;
				bv_append(&out, "\n", 1);
				i = line_end;
				if (i < n && src[i] == '\n')
					++i;
				continue;
			}

			/* 以下指令仅在 active 时处理 */
			if (!cond_active()) {
				bv_append(&out, "\n", 1);
				i = line_end;
				if (i < n && src[i] == '\n')
					++i;
				continue;
			}

			if (u_streq(directive, "define")) {
				/* #define NAME value  或  #define NAME(params)
				 * body */
				while (q < line_end &&
				       (src[q] == ' ' || src[q] == '\t'))
					++q;
				unsigned long nm_start = q;
				while (q < line_end &&
				       is_ident_start((unsigned char)src[q]))
					++q;
				unsigned long nm_end = q;
				while (q < line_end &&
				       (src[q] == ' ' || src[q] == '\t'))
					++q;
				/* function-like macro? NAME( */
				if (q < line_end && src[q] == '(' &&
				    nm_end > nm_start) {
					/* 跳过 function-like macro 定义 */
					/* 找到匹配的右括号 */
					int pdepth = 1;
					++q;
					while (q < line_end && pdepth > 0) {
						if (src[q] == '(')
							++pdepth;
						else if (src[q] == ')')
							--pdepth;
						++q;
					}
					/* 跳过空格，取 body */
					while (q < line_end && (src[q] == ' ' ||
								src[q] == '\t'))
						++q;
					/* 注册为空 object-like（不展开） */
					if (nm_end > nm_start) {
						char nm[64];
						unsigned long nl =
						    nm_end - nm_start;
						if (nl > 63)
							nl = 63;
						mc_memcpy(
						    nm, src + nm_start, nl);
						nm[nl] = 0;
						macro_add(nm, 0);
					}
				} else {
					/* object-like macro */
					if (nm_end > nm_start) {
						char nm[64];
						unsigned long nl =
						    nm_end - nm_start;
						if (nl > 63)
							nl = 63;
						mc_memcpy(
						    nm, src + nm_start, nl);
						nm[nl] = 0;
						/* 取 value */
						unsigned long v_start = q;
						unsigned long v_end = line_end;
						while (
						    v_end > v_start &&
						    (src[v_end - 1] == ' ' ||
						     src[v_end - 1] == '\t' ||
						     src[v_end - 1] == '\r'))
							--v_end;
						if (v_end > v_start) {
							char vb[512];
							unsigned long vl =
							    v_end - v_start;
							if (vl > 511)
								vl = 511;
							mc_memcpy(vb,
								  src + v_start,
								  vl);
							vb[vl] = 0;
							macro_add(nm, vb);
						} else {
							macro_add(nm, 0);
						}
					}
				}
				bv_append(&out, "\n", 1);
				i = line_end;
				if (i < n && src[i] == '\n')
					++i;
				continue;
			}
			if (u_streq(directive, "undef")) {
				while (q < line_end &&
				       (src[q] == ' ' || src[q] == '\t'))
					++q;
				unsigned long nm_start = q;
				while (q < line_end &&
				       is_ident_char((unsigned char)src[q]))
					++q;
				if (q > nm_start) {
					char nm[64];
					unsigned long nl = q - nm_start;
					if (nl > 63)
						nl = 63;
					mc_memcpy(nm, src + nm_start, nl);
					nm[nl] = 0;
					macro_remove(nm);
				}
				bv_append(&out, "\n", 1);
				i = line_end;
				if (i < n && src[i] == '\n')
					++i;
				continue;
			}
			if (u_streq(directive, "include")) {
				/* #include <file> or #include "file" */
				while (q < line_end &&
				       (src[q] == ' ' || src[q] == '\t'))
					++q;
				if (q >= line_end ||
				    (src[q] != '<' && src[q] != '"'))
					die_plain("lcc: malformed #include");
				unsigned char closing =
				    src[q] == '<' ? '>' : '"';
				++q;
				unsigned long name_start2 = q;
				while (q < line_end && src[q] != closing)
					++q;
				if (q >= line_end)
					die_plain("lcc: malformed #include");
				{
					char* name2 = xstrdup_n(
					    (const char*)(src + name_start2),
					    q - name_start2);
					char* inc2 =
					    join_path(g_include_dir, name2);
					char* child2;
					free(name2);
					if (!inc2)
						die_plain("lcc: out of memory");
					child2 = expand_file(inc2, depth + 1);
					free(inc2);
					if (!child2)
						die_plain(
						    "lcc: include failed");
					if (bv_append(&out,
						      child2,
						      u_strlen(child2)) < 0)
						die_plain("lcc: out of memory");
					if (bv_append(&out, "\n", 1) < 0)
						die_plain("lcc: out of memory");
					free(child2);
				}
				i = line_end;
				if (i < n && src[i] == '\n')
					++i;
				continue;
			}
			/* #error, #pragma, #line, #if, #elif — 跳过 */
			bv_append(&out, "\n", 1);
			i = line_end;
			if (i < n && src[i] == '\n')
				++i;
			continue;
		}

		/* ---- 非指令行 ---- */
		if (cond_active()) {
			/* 宏替换后输出 */
			char* substituted =
			    macro_substitute((const char*)(src + line_start),
					     line_end - line_start);
			if (!substituted)
				die_plain("lcc: out of memory");
			if (bv_append(
				&out, substituted, u_strlen(substituted)) < 0)
				die_plain("lcc: out of memory");
			free(substituted);
			if (line_end < n && src[line_end] == '\n') {
				if (bv_append(&out, "\n", 1) < 0)
					die_plain("lcc: out of memory");
			}
		} else {
			/* inactive 行：输出空行保持行号 */
			if (line_end < n && src[line_end] == '\n')
				bv_append(&out, "\n", 1);
		}
		i = line_end;
		if (i < n && src[i] == '\n')
			++i;
	}

	if (depth > 32)
		die_plain("lcc: include nesting too deep");
	if (bv_append(&out, "", 1) < 0)
		die_plain("lcc: out of memory");
	return (char*)out.p;
}

static char* expand_file(const char* path, int depth)
{
	unsigned char* buf;
	unsigned long n;
	char* r;
	if (depth > 32)
		return 0;
	if (file_read_all(path, &buf, &n) < 0)
		return 0;
	r = expand_source_text(path, buf, n, depth);
	free(buf);
	return r;
}

enum TokenKind {
	TOK_EOF,
	TOK_ID,
	TOK_NUM,
	TOK_CHAR,
	TOK_STRING,
	TOK_KEYWORD,
	TOK_OP
};

struct Token {
	enum TokenKind kind;
	char* text;
	unsigned long len; /* Needed because string literals may contain NUL. */
	unsigned long long value;
	int line;
	int col;
};

struct TokenVec {
	struct Token* v;
	unsigned long n;
	unsigned long cap;
};

static void tv_init(struct TokenVec* v)
{
	v->v = 0;
	v->n = 0;
	v->cap = 0;
}

static int tv_push(struct TokenVec* v,
		   enum TokenKind k,
		   const char* s,
		   unsigned long len,
		   unsigned long long value,
		   int line,
		   int col)
{
	struct Token* p;
	if (v->n == v->cap) {
		unsigned long nc = v->cap ? v->cap * 2 : 256;
		p = (struct Token*)realloc(v->v, nc * sizeof(*p));
		if (!p)
			return -1;
		v->v = p;
		v->cap = nc;
	}
	v->v[v->n].kind = k;
	v->v[v->n].text = xstrdup_n(s ? s : "", len);
	v->v[v->n].len = len;
	v->v[v->n].value = value;
	v->v[v->n].line = line;
	v->v[v->n].col = col;
	if (!v->v[v->n].text)
		return -1;
	++v->n;
	return 0;
}

static int is_keyword(const char* s, unsigned long len)
{
	static const char* kw[] = {"int",
				   "long",
				   "char",
				   "void",
				   "if",
				   "else",
				   "while",
				   "for",
				   "return",
				   "break",
				   "continue",
				   "sizeof"};
	unsigned long i;
	for (i = 0; i < sizeof(kw) / sizeof(kw[0]); ++i) {
		unsigned long n = u_strlen(kw[i]);
		if (n == len && !u_strncmp_n(s, kw[i], n))
			return 1;
	}
	return 0;
}

static int hexval(int c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

static unsigned long long
parse_esc(const char** pp, const char* end, int line, int col, const char* file)
{
	const char* p = *pp;
	int c;
	if (p >= end)
		dief(file, line, col, "unterminated escape sequence");
	c = (unsigned char)*p++;
	switch (c) {
	case 'n':
		*pp = p;
		return '\n';
	case 't':
		*pp = p;
		return '\t';
	case 'r':
		*pp = p;
		return '\r';
	case '0':
		*pp = p;
		return 0;
	case '\\':
		*pp = p;
		return '\\';
	case '"':
		*pp = p;
		return '"';
	case '\'':
		*pp = p;
		return '\'';
	case 'x': {
		int h1, h2;
		if (p >= end)
			dief(file, line, col, "bad hex escape");
		h1 = hexval((unsigned char)*p++);
		if (p >= end)
			dief(file, line, col, "bad hex escape");
		h2 = hexval((unsigned char)*p++);
		if (h1 < 0 || h2 < 0)
			dief(file, line, col, "bad hex escape");
		*pp = p;
		return (unsigned long long)((h1 << 4) | h2);
	}
	default:
		dief(file, line, col, "invalid escape sequence");
	}
	return 0;
}

static int lex_all(const char* file, const char* src, struct TokenVec* out)
{
	const char* p = src;
	int line = 1, col = 1;
	tv_init(out);

	for (;;) {
		while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
			if (*p == '\n') {
				++line;
				col = 1;
			} else
				++col;
			++p;
		}
		if (!*p) {
			return tv_push(out, TOK_EOF, "", 0, 0, line, col);
		}

		if (p[0] == '/' && p[1] == '/') {
			p += 2;
			col += 2;
			while (*p && *p != '\n') {
				++p;
				++col;
			}
			continue;
		}
		if (p[0] == '/' && p[1] == '*') {
			p += 2;
			col += 2;
			while (*p) {
				if (p[0] == '*' && p[1] == '/') {
					p += 2;
					col += 2;
					break;
				}
				if (*p == '\n') {
					++line;
					col = 1;
					++p;
				} else {
					++col;
					++p;
				}
			}
			if (!*p && !(p[-2] == '*' && p[-1] == '/'))
				dief(file, line, col, "unterminated comment");
			continue;
		}

		if (mc_is_alpha((unsigned char)*p) || *p == '_') {
			const char* s = p;
			int sc = col;
			while (mc_is_alnum((unsigned char)*p) || *p == '_') {
				++p;
				++col;
			}
			if (tv_push(out,
				    is_keyword(s, (unsigned long)(p - s))
					? TOK_KEYWORD
					: TOK_ID,
				    s,
				    (unsigned long)(p - s),
				    0,
				    line,
				    sc) < 0)
				die_plain("lcc: out of memory");
			continue;
		}

		if (mc_is_digit((unsigned char)*p)) {
			const char* s = p;
			int sc = col;
			int base = 10;
			unsigned long long value = 0;
			if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
				base = 16;
				p += 2;
				col += 2;
				while (mc_is_xdigit((unsigned char)*p)) {
					int h = hexval((unsigned char)*p++);
					++col;
					value = value * 16 + (unsigned)h;
				}
			} else {
				while (mc_is_digit((unsigned char)*p)) {
					value =
					    value * 10 + (unsigned)(*p - '0');
					++p;
					++col;
				}
			}
			if (*p == 'L') {
				++p;
				++col;
			}
			(void)base;
			if (tv_push(out,
				    TOK_NUM,
				    s,
				    (unsigned long)(p - s),
				    value,
				    line,
				    sc) < 0)
				die_plain("lcc: out of memory");
			continue;
		}

		if (*p == '\'') {
			int sc = col;
			const char* q = ++p;
			++col;
			unsigned long long v;
			if (!*q)
				dief(file,
				     line,
				     sc,
				     "unterminated character literal");
			if (*q == '\\') {
				++p;
				++col;
				v = parse_esc(
				    &p, src + u_strlen(src), line, sc, file);
				col += 0;
			} else {
				v = (unsigned char)*p++;
				++col;
			}
			if (*p != '\'')
				dief(file,
				     line,
				     sc,
				     "character literal must contain one "
				     "character");
			++p;
			++col;
			if (tv_push(out, TOK_CHAR, "", 0, v, line, sc) < 0)
				die_plain("lcc: out of memory");
			continue;
		}

		if (*p == '"') {
			int sc = col;
			const char* q = ++p;
			++col;
			struct ByteVec str;
			bv_init(&str);
			while (*p && *p != '"') {
				unsigned char ch;
				if (*p == '\\') {
					++p;
					++col;
					ch = (unsigned char)parse_esc(
					    &p,
					    src + u_strlen(src),
					    line,
					    sc,
					    file);
				} else {
					ch = (unsigned char)*p++;
					++col;
				}
				if (bv_append(&str, &ch, 1) < 0)
					die_plain("lcc: out of memory");
			}
			if (*p != '"')
				dief(file,
				     line,
				     sc,
				     "unterminated string literal");
			++p;
			++col;
			if (tv_push(out,
				    TOK_STRING,
				    (const char*)str.p,
				    str.n,
				    0,
				    line,
				    sc) < 0)
				die_plain("lcc: out of memory");
			free(str.p);
			(void)q;
			continue;
		}

		{
			static const char* ops[] = {
			    "<<=", ">>=", "==", "!=", "<=", ">=", "<<", ">>",
			    "&&",  "||",  "++", "--", "+=", "-=", "*=", "/=",
			    "%=",  "&=",  "|=", "^=", "->", "..."};
			unsigned long oi;
			int found = 0;
			for (oi = 0; oi < sizeof(ops) / sizeof(ops[0]); ++oi) {
				unsigned long m = u_strlen(ops[oi]);
				if (!u_strncmp_n(p, ops[oi], m)) {
					int sc = col;
					p += m;
					col += (int)m;
					if (tv_push(out,
						    TOK_OP,
						    ops[oi],
						    m,
						    0,
						    line,
						    sc) < 0)
						die_plain("lcc: out of memory");
					found = 1;
					break;
				}
			}
			if (found)
				continue;
		}

		if (mc_char_in("+-*/%&|^~!=<>()[]{};,?:.", *p)) {
			int sc = col;
			char op[2];
			op[0] = *p;
			op[1] = 0;
			++p;
			++col;
			if (tv_push(out, TOK_OP, op, 1, 0, line, sc) < 0)
				die_plain("lcc: out of memory");
			continue;
		}

		dief(file, line, col, "illegal character");
	}
}

enum BaseType { BT_VOID, BT_CHAR, BT_INT, BT_LONG };

struct Type {
	enum BaseType base;
	int ptr;
};

static unsigned long type_size(struct Type t)
{
	if (t.ptr)
		return 8;
	if (t.base == BT_CHAR)
		return 1;
	if (t.base == BT_INT)
		return 4;
	if (t.base == BT_LONG)
		return 8;
	return 0;
}

static int type_equal(struct Type a, struct Type b)
{
	return a.base == b.base && a.ptr == b.ptr;
}

struct Symbol {
	char* name;
	struct Type type;
	int is_function;
	int defined;
	int variadic;
	struct Type* params;
	int nparams;
	long long stack_off;
	int section; /* 0 undef, 1 text, 2 data, 3 bss, SHN_ABS otherwise */
	unsigned long long value;
	unsigned long long size;
};

struct SymVec {
	struct Symbol** v;
	int n;
	int cap;
};

static struct Symbol* sym_new(const char* name, unsigned long len)
{
	struct Symbol* s = (struct Symbol*)xcalloc(1, sizeof(*s));
	if (!s)
		return 0;
	s->name = xstrdup_n(name, len);
	if (!s->name) {
		free(s);
		return 0;
	}
	return s;
}

static int sv_push(struct SymVec* v, struct Symbol* s)
{
	if (v->n == v->cap) {
		int nc = v->cap ? v->cap * 2 : 64;
		struct Symbol** p =
		    (struct Symbol**)realloc(v->v, nc * sizeof(*p));
		if (!p)
			return -1;
		v->v = p;
		v->cap = nc;
	}
	v->v[v->n++] = s;
	return 0;
}

static struct Symbol* sym_find(struct SymVec* v, const char* name)
{
	int i;
	for (i = v->n - 1; i >= 0; --i) {
		if (u_streq(v->v[i]->name, name))
			return v->v[i];
	}
	return 0;
}

enum ExprKind {
	EX_INT,
	EX_CHAR,
	EX_STRING,
	EX_VAR,
	EX_UNARY,
	EX_BINARY,
	EX_ASSIGN,
	EX_CALL,
	EX_INDEX,
	EX_COND,
	EX_SIZEOF
};

enum StmtKind {
	ST_BLOCK,
	ST_DECL,
	ST_EXPR,
	ST_IF,
	ST_WHILE,
	ST_FOR,
	ST_RETURN,
	ST_BREAK,
	ST_CONTINUE
};

struct Expr;
struct Stmt;

struct ExprVec {
	struct Expr** v;
	int n;
	int cap;
};

struct StmtVec {
	struct Stmt** v;
	int n;
	int cap;
};

struct Expr {
	enum ExprKind kind;
	struct Type type;
	int line, col;
	unsigned long long value;
	char* text;
	unsigned long text_len;
	char* op;
	struct Expr *a, *b, *c;
	struct ExprVec args;
	struct Symbol* sym;
};

struct Stmt {
	enum StmtKind kind;
	int line, col;
	struct Symbol* sym;
	struct Expr *e, *e2, *e3;
	struct Stmt *s1, *s2;
	struct StmtVec body;
};

static struct Expr* expr_new(enum ExprKind k, int line, int col)
{
	struct Expr* e = (struct Expr*)xcalloc(1, sizeof(*e));
	if (!e)
		die_plain("lcc: out of memory");
	e->kind = k;
	e->line = line;
	e->col = col;
	return e;
}

static struct Stmt* stmt_new(enum StmtKind k, int line, int col)
{
	struct Stmt* s = (struct Stmt*)xcalloc(1, sizeof(*s));
	if (!s)
		die_plain("lcc: out of memory");
	s->kind = k;
	s->line = line;
	s->col = col;
	return s;
}

static void exprvec_push(struct ExprVec* v, struct Expr* e)
{
	if (v->n == v->cap) {
		int nc = v->cap ? v->cap * 2 : 8;
		struct Expr** p = (struct Expr**)realloc(v->v, nc * sizeof(*p));
		if (!p)
			die_plain("lcc: out of memory");
		v->v = p;
		v->cap = nc;
	}
	v->v[v->n++] = e;
}

static void stmtvec_push(struct StmtVec* v, struct Stmt* s)
{
	if (v->n == v->cap) {
		int nc = v->cap ? v->cap * 2 : 8;
		struct Stmt** p = (struct Stmt**)realloc(v->v, nc * sizeof(*p));
		if (!p)
			die_plain("lcc: out of memory");
		v->v = p;
		v->cap = nc;
	}
	v->v[v->n++] = s;
}

struct LocalScope {
	struct SymVec syms;
};

struct FunctionDef {
	struct Symbol* sym;
	struct Symbol** params;
	int nparams;
	struct Stmt* body;
};

struct FuncVec {
	struct FunctionDef* v;
	int n;
	int cap;
};

static void fv_push(struct FuncVec* v, struct FunctionDef f)
{
	if (v->n == v->cap) {
		int nc = v->cap ? v->cap * 2 : 16;
		struct FunctionDef* p =
		    (struct FunctionDef*)realloc(v->v, nc * sizeof(*p));
		if (!p)
			die_plain("lcc: out of memory");
		v->v = p;
		v->cap = nc;
	}
	v->v[v->n++] = f;
}

struct GlobalInit {
	struct Symbol* sym;
	struct Expr* init;
};

struct GlobalVec {
	struct GlobalInit* v;
	int n;
	int cap;
};

static void gv_push(struct GlobalVec* v, struct GlobalInit g)
{
	if (v->n == v->cap) {
		int nc = v->cap ? v->cap * 2 : 32;
		struct GlobalInit* p =
		    (struct GlobalInit*)realloc(v->v, nc * sizeof(*p));
		if (!p)
			die_plain("lcc: out of memory");
		v->v = p;
		v->cap = nc;
	}
	v->v[v->n++] = g;
}

struct Parser {
	const char* file;
	struct TokenVec* tv;
	unsigned long pos;
	struct SymVec globals;
	struct LocalScope* scopes;
	int nscopes, cscopes;
	struct FuncVec funcs;
	struct GlobalVec globals_init;
	int loop_depth;
	struct Type current_return_type;
};

static struct Token* ptok(struct Parser* p)
{
	return &p->tv->v[p->pos];
}
static int is_tok(struct Parser* p, const char* s)
{
	struct Token* t = ptok(p);
	return t->len == u_strlen(s) && !u_strncmp_n(t->text, s, t->len);
}
static int eat_tok(struct Parser* p, const char* s)
{
	if (is_tok(p, s)) {
		++p->pos;
		return 1;
	}
	return 0;
}

static void perr(struct Parser* p, const char* msg)
{
	dief(p->file, ptok(p)->line, ptok(p)->col, msg);
}

static void expect_tok(struct Parser* p, const char* s)
{
	if (!eat_tok(p, s))
		perr(p, "unexpected token");
}

static void parser_push_scope(struct Parser* p)
{
	if (p->nscopes == p->cscopes) {
		int nc = p->cscopes ? p->cscopes * 2 : 8;
		struct LocalScope* q =
		    (struct LocalScope*)realloc(p->scopes, nc * sizeof(*q));
		if (!q)
			die_plain("lcc: out of memory");
		p->scopes = q;
		p->cscopes = nc;
	}
	mc_memset(&p->scopes[p->nscopes++], 0, sizeof(p->scopes[0]));
}

static void parser_pop_scope(struct Parser* p)
{
	--p->nscopes;
}

static struct Symbol* lookup_parser(struct Parser* p, const char* name)
{
	int i, j;
	for (i = p->nscopes - 1; i >= 0; --i) {
		for (j = p->scopes[i].syms.n - 1; j >= 0; --j) {
			if (u_streq(p->scopes[i].syms.v[j]->name, name))
				return p->scopes[i].syms.v[j];
		}
	}
	return sym_find(&p->globals, name);
}

static int scope_has(struct LocalScope* s, const char* name)
{
	int i;
	for (i = 0; i < s->syms.n; ++i)
		if (u_streq(s->syms.v[i]->name, name))
			return 1;
	return 0;
}

static void add_local(struct Parser* p, struct Symbol* s)
{
	if (scope_has(&p->scopes[p->nscopes - 1], s->name))
		perr(p, "duplicate declaration");
	if (sv_push(&p->scopes[p->nscopes - 1].syms, s) < 0)
		die_plain("lcc: out of memory");
}

static int is_typedef_name(const char* s)
{
	static const char* names[] = {
	    "size_t",	"ssize_t",   "ptrdiff_t", "intptr_t", "uintptr_t",
	    "int8_t",	"uint8_t",   "int16_t",	  "uint16_t", "int32_t",
	    "uint32_t", "int64_t",   "uint64_t",  "off_t",    "ino_t",
	    "dev_t",	"pid_t",     "mode_t",	  "uid_t",    "gid_t",
	    "time_t",	"blksize_t", "blkcnt_t",  "nlink_t",  "FILE",
	    "voidp_t"};
	unsigned long i;
	for (i = 0; i < sizeof(names) / sizeof(names[0]); ++i)
		if (u_streq(s, names[i]))
			return 1;
	return 0;
}

static int token_is_type_word(struct Parser* p)
{
	struct Token* t = ptok(p);
	if (t->kind == TOK_KEYWORD && (is_tok(p, "int") || is_tok(p, "long") ||
				       is_tok(p, "char") || is_tok(p, "void")))
		return 1;
	if (t->kind == TOK_ID && is_typedef_name(t->text))
		return 1;
	if (t->kind == TOK_KEYWORD &&
	    (is_tok(p, "const") || is_tok(p, "unsigned") ||
	     is_tok(p, "signed") || is_tok(p, "short")))
		return 1;
	if (t->kind == TOK_ID && is_tok(p, "struct"))
		return 1;
	return 0;
}

static struct Type parse_type(struct Parser* p)
{
	struct Type t;
	int saw_long = 0;
	t.base = BT_INT;
	t.ptr = 0;

	/* Header files may use qualifiers that MiniC itself does not expose.
	 * They do not change our simplified ABI model. */
	for (;;) {
		if (is_tok(p, "const") || is_tok(p, "volatile") ||
		    is_tok(p, "restrict") || is_tok(p, "signed") ||
		    is_tok(p, "unsigned") || is_tok(p, "short")) {
			++p->pos;
			continue;
		}
		if (is_tok(p, "long")) {
			saw_long = 1;
			++p->pos;
			continue;
		}
		break;
	}

	if (is_tok(p, "struct")) {
		/* Opaque structure type. Struct objects are not manipulable by
		 * MiniC, but struct pointers are common in the supplied
		 * headers. */
		++p->pos;
		if (ptok(p)->kind == TOK_ID)
			++p->pos;
		t.base = BT_LONG;
	} else if (is_tok(p, "void")) {
		t.base = BT_VOID;
		++p->pos;
	} else if (is_tok(p, "char")) {
		t.base = BT_CHAR;
		++p->pos;
	} else if (is_tok(p, "int")) {
		t.base = BT_INT;
		++p->pos;
	} else if (is_tok(p, "long") || saw_long) {
		t.base = BT_LONG;
		if (is_tok(p, "long"))
			++p->pos;
	} else if (ptok(p)->kind == TOK_ID && is_typedef_name(ptok(p)->text)) {
		t.base = BT_LONG;
		++p->pos;
	} else {
		perr(p, "expected type");
	}

	while (eat_tok(p, "*"))
		++t.ptr;
	while (is_tok(p, "restrict"))
		++p->pos;
	return t;
}

static struct Expr* parse_expression(struct Parser* p);

static struct Expr* parse_primary(struct Parser* p)
{
	struct Token* t = ptok(p);
	struct Expr* e;
	if (t->kind == TOK_NUM) {
		e = expr_new(EX_INT, t->line, t->col);
		e->value = t->value;
		e->type.base = BT_LONG;
		e->type.ptr = 0;
		++p->pos;
		return e;
	}
	if (t->kind == TOK_CHAR) {
		e = expr_new(EX_CHAR, t->line, t->col);
		e->value = t->value;
		e->type.base = BT_CHAR;
		++p->pos;
		return e;
	}
	if (t->kind == TOK_STRING) {
		e = expr_new(EX_STRING, t->line, t->col);
		e->text = xstrdup_n(t->text, t->len);
		e->text_len = t->len;
		e->type.base = BT_CHAR;
		e->type.ptr = 1;
		++p->pos;
		return e;
	}
	if (t->kind == TOK_ID) {
		struct Symbol* sym;
		char* name = xstrdup_n(t->text, t->len);
		++p->pos;
		sym = lookup_parser(p, name);
		if (!sym)
			perr(p, "undeclared identifier");
		if (eat_tok(p, "(")) {
			e = expr_new(EX_CALL, t->line, t->col);
			e->sym = sym;
			e->type = sym->type;
			if (!sym->is_function)
				perr(p, "called object is not a function");
			if (!eat_tok(p, ")")) {
				for (;;) {
					if (is_tok(p, "..."))
						perr(p,
						     "variadic arguments are "
						     "not supported");
					exprvec_push(&e->args,
						     parse_expression(p));
					if (eat_tok(p, ")"))
						break;
					expect_tok(p, ",");
				}
			}
			if (!sym->variadic && e->args.n != sym->nparams)
				perr(p, "function argument count mismatch");
			if (sym->variadic && e->args.n < sym->nparams)
				perr(p, "too few arguments");
			free(name);
			return e;
		}
		e = expr_new(EX_VAR, t->line, t->col);
		e->sym = sym;
		e->type = sym->type;
		free(name);
		return e;
	}
	if (eat_tok(p, "(")) {
		e = parse_expression(p);
		expect_tok(p, ")");
		return e;
	}
	perr(p, "expected expression");
	return 0;
}

static struct Expr* parse_postfix(struct Parser* p)
{
	struct Expr* e = parse_primary(p);
	for (;;) {
		if (eat_tok(p, "[")) {
			struct Expr* idx = parse_expression(p);
			expect_tok(p, "]");
			if (!e->type.ptr)
				perr(p, "subscript requires pointer");
			{
				struct Expr* z =
				    expr_new(EX_INDEX, e->line, e->col);
				z->a = e;
				z->b = idx;
				z->type = e->type;
				--z->type.ptr;
				e = z;
			}
			continue;
		}
		if (eat_tok(p, "++")) {
			struct Expr* z = expr_new(EX_UNARY, e->line, e->col);
			z->op = xstrdup_n("post++", 6);
			z->a = e;
			z->type = e->type;
			e = z;
			continue;
		}
		if (eat_tok(p, "--")) {
			struct Expr* z = expr_new(EX_UNARY, e->line, e->col);
			z->op = xstrdup_n("post--", 6);
			z->a = e;
			z->type = e->type;
			e = z;
			continue;
		}
		if (is_tok(p, ".") || is_tok(p, "->"))
			perr(p, "struct/union member access is not supported");
		break;
	}
	return e;
}

static struct Expr* parse_unary(struct Parser* p)
{
	const char* ops[] = {"+", "-", "!", "~", "*", "&", "++", "--"};
	unsigned long i;
	if (is_tok(p, "sizeof")) {
		int l = ptok(p)->line, c = ptok(p)->col;
		++p->pos;
		{
			struct Expr* e = expr_new(EX_SIZEOF, l, c);
			if (eat_tok(p, "(")) {
				if (token_is_type_word(p)) {
					struct Type t = parse_type(p);
					expect_tok(p, ")");
					e->value = type_size(t);
					e->type.base = BT_LONG;
					return e;
				}
				e->a = parse_expression(p);
				expect_tok(p, ")");
				e->value = type_size(e->a->type);
				e->type.base = BT_LONG;
				return e;
			}
			e->a = parse_unary(p);
			e->value = type_size(e->a->type);
			e->type.base = BT_LONG;
			return e;
		}
	}
	for (i = 0; i < sizeof(ops) / sizeof(ops[0]); ++i)
		if (is_tok(p, ops[i])) {
			int l = ptok(p)->line, c = ptok(p)->col;
			struct Expr* e = expr_new(EX_UNARY, l, c);
			e->op = xstrdup_n(ops[i], u_strlen(ops[i]));
			++p->pos;
			e->a = parse_unary(p);
			e->type = e->a->type;
			if (ops[i][0] == '&')
				++e->type.ptr;
			if (ops[i][0] == '*') {
				if (!e->a->type.ptr)
					perr(p,
					     "cannot dereference non-pointer");
				--e->type.ptr;
			}
			return e;
		}
	return parse_postfix(p);
}

static int prec(const char* s)
{
	if (!u_streq(s, "*") && !u_streq(s, "/") && !u_streq(s, "%") &&
	    !u_streq(s, "+") && !u_streq(s, "-") && !u_streq(s, "<<") &&
	    !u_streq(s, ">>") && !u_streq(s, "<") && !u_streq(s, "<=") &&
	    !u_streq(s, ">") && !u_streq(s, ">=") && !u_streq(s, "==") &&
	    !u_streq(s, "!=") && !u_streq(s, "&") && !u_streq(s, "^") &&
	    !u_streq(s, "|") && !u_streq(s, "&&") && !u_streq(s, "||"))
		return -1;
	if (u_streq(s, "*") || u_streq(s, "/") || u_streq(s, "%"))
		return 70;
	if (u_streq(s, "+") || u_streq(s, "-"))
		return 60;
	if (u_streq(s, "<<") || u_streq(s, ">>"))
		return 50;
	if (u_streq(s, "<") || u_streq(s, "<=") || u_streq(s, ">") ||
	    u_streq(s, ">="))
		return 40;
	if (u_streq(s, "==") || u_streq(s, "!="))
		return 35;
	if (u_streq(s, "&"))
		return 30;
	if (u_streq(s, "^"))
		return 25;
	if (u_streq(s, "|"))
		return 20;
	if (u_streq(s, "&&"))
		return 15;
	return 10;
}

static struct Expr* parse_binary(struct Parser* p, int minp)
{
	struct Expr* lhs = parse_unary(p);
	for (;;) {
		int pp = prec(ptok(p)->text);
		char* op;
		if (pp < minp)
			break;
		op = xstrdup_n(ptok(p)->text, ptok(p)->len);
		++p->pos;
		{
			struct Expr* rhs = parse_binary(p, pp + 1);
			struct Expr* z =
			    expr_new(EX_BINARY, lhs->line, lhs->col);
			z->op = op;
			z->a = lhs;
			z->b = rhs;
			z->type.base = BT_LONG;
			lhs = z;
		}
	}
	return lhs;
}

static struct Expr* parse_expression(struct Parser* p)
{
	struct Expr* lhs = parse_binary(p, 0);
	if (is_tok(p, "=") || is_tok(p, "+=") || is_tok(p, "-=") ||
	    is_tok(p, "*=") || is_tok(p, "/=") || is_tok(p, "%=") ||
	    is_tok(p, "&=") || is_tok(p, "|=") || is_tok(p, "^=") ||
	    is_tok(p, "<<=") || is_tok(p, ">>=")) {
		struct Expr* z = expr_new(EX_ASSIGN, lhs->line, lhs->col);
		z->op = xstrdup_n(ptok(p)->text, ptok(p)->len);
		++p->pos;
		z->a = lhs;
		z->b = parse_expression(p);
		z->type = lhs->type;
		lhs = z;
	}
	if (eat_tok(p, "?")) {
		struct Expr* yes = parse_expression(p);
		expect_tok(p, ":");
		{
			struct Expr* no = parse_expression(p);
			struct Expr* z = expr_new(EX_COND, lhs->line, lhs->col);
			z->a = lhs;
			z->b = yes;
			z->c = no;
			z->type = yes->type;
			lhs = z;
		}
	}
	return lhs;
}

static struct Stmt* parse_statement(struct Parser* p);

static struct Stmt* parse_decl_statement(struct Parser* p,
					 int consume_semicolon)
{
	struct Stmt* block = stmt_new(ST_BLOCK, ptok(p)->line, ptok(p)->col);
	struct Type t = parse_type(p);
	for (;;) {
		if (ptok(p)->kind != TOK_ID)
			perr(p, "expected variable name");
		struct Symbol* s = sym_new(ptok(p)->text, ptok(p)->len);
		struct Stmt* d;
		int dl = ptok(p)->line, dc = ptok(p)->col;
		if (!s)
			die_plain("lcc: out of memory");
		s->type = t;
		s->is_function = 0;
		add_local(p, s);
		++p->pos;
		d = stmt_new(ST_DECL, dl, dc);
		d->sym = s;
		if (eat_tok(p, "="))
			d->e = parse_expression(p);
		stmtvec_push(&block->body, d);
		if (!eat_tok(p, ","))
			break;
	}
	if (consume_semicolon)
		expect_tok(p, ";");
	return block;
}

static struct Stmt* parse_statement(struct Parser* p)
{
	int l = ptok(p)->line, c = ptok(p)->col;
	if (eat_tok(p, "{")) {
		struct Stmt* s = stmt_new(ST_BLOCK, l, c);
		parser_push_scope(p);
		while (!eat_tok(p, "}")) {
			if (ptok(p)->kind == TOK_EOF)
				perr(p, "unterminated block");
			stmtvec_push(&s->body, parse_statement(p));
		}
		parser_pop_scope(p);
		return s;
	}
	if (token_is_type_word(p) && !is_tok(p, "void"))
		return parse_decl_statement(p, 1);
	if (eat_tok(p, "if")) {
		struct Stmt* s = stmt_new(ST_IF, l, c);
		expect_tok(p, "(");
		s->e = parse_expression(p);
		expect_tok(p, ")");
		s->s1 = parse_statement(p);
		if (eat_tok(p, "else"))
			s->s2 = parse_statement(p);
		return s;
	}
	if (eat_tok(p, "while")) {
		struct Stmt* s = stmt_new(ST_WHILE, l, c);
		expect_tok(p, "(");
		s->e = parse_expression(p);
		expect_tok(p, ")");
		++p->loop_depth;
		s->s1 = parse_statement(p);
		--p->loop_depth;
		return s;
	}
	if (eat_tok(p, "for")) {
		struct Stmt* s = stmt_new(ST_FOR, l, c);
		expect_tok(p, "(");
		if (token_is_type_word(p) && !is_tok(p, "void"))
			s->s1 = parse_decl_statement(p, 0);
		else if (!is_tok(p, ";"))
			s->s1 = stmt_new(ST_EXPR, l, c),
			s->s1->e = parse_expression(p);
		expect_tok(p, ";");
		if (!is_tok(p, ";"))
			s->e = parse_expression(p);
		expect_tok(p, ";");
		if (!is_tok(p, ")"))
			s->e2 = parse_expression(p);
		expect_tok(p, ")");
		++p->loop_depth;
		s->s2 = parse_statement(p);
		--p->loop_depth;
		return s;
	}
	if (eat_tok(p, "return")) {
		struct Stmt* s = stmt_new(ST_RETURN, l, c);
		if (!is_tok(p, ";"))
			s->e = parse_expression(p);
		if (p->current_return_type.base == BT_VOID &&
		    p->current_return_type.ptr == 0 && s->e)
			perr(p, "void function cannot return a value");
		expect_tok(p, ";");
		return s;
	}
	if (eat_tok(p, "break")) {
		struct Stmt* s = stmt_new(ST_BREAK, l, c);
		if (!p->loop_depth)
			perr(p, "break outside loop");
		expect_tok(p, ";");
		return s;
	}
	if (eat_tok(p, "continue")) {
		struct Stmt* s = stmt_new(ST_CONTINUE, l, c);
		if (!p->loop_depth)
			perr(p, "continue outside loop");
		expect_tok(p, ";");
		return s;
	}
	{
		struct Stmt* s = stmt_new(ST_EXPR, l, c);
		s->e = parse_expression(p);
		expect_tok(p, ";");
		return s;
	}
}

static int signature_equal(
    struct Symbol* s, struct Type ret, struct Type* params, int n, int variadic)
{
	int i;
	if (!s || !s->is_function || !type_equal(s->type, ret) ||
	    s->nparams != n || s->variadic != variadic)
		return 0;
	for (i = 0; i < n; ++i)
		if (!type_equal(s->params[i], params[i]))
			return 0;
	return 1;
}

static struct Symbol* get_or_create_function(struct Parser* p,
					     const char* name,
					     unsigned long len,
					     struct Type ret,
					     struct Type* params,
					     int n,
					     int variadic)
{
	struct Symbol* s = sym_find(&p->globals, name);
	if (!s) {
		s = sym_new(name, len);
		if (!s)
			die_plain("lcc: out of memory");
		s->is_function = 1;
		s->type = ret;
		s->variadic = variadic;
		s->nparams = n;
		if (n) {
			s->params = (struct Type*)xmalloc(n * sizeof(*params));
			if (!s->params)
				die_plain("lcc: out of memory");
			mc_memcpy(s->params, params, n * sizeof(*params));
		}
		sv_push(&p->globals, s);
		return s;
	}
	if (!signature_equal(s, ret, params, n, variadic))
		perr(p, "conflicting function declaration");
	return s;
}

static void parser_init(struct Parser* p, const char* file, struct TokenVec* tv)
{
	mc_memset(p, 0, sizeof(*p));
	p->file = file;
	p->tv = tv;
}

static void skip_storage_prefix(struct Parser* p)
{
	while (is_tok(p, "extern") || is_tok(p, "static") ||
	       is_tok(p, "inline") || is_tok(p, "__inline__"))
		++p->pos;
}

static void parse_translation_unit(struct Parser* p)
{
	while (ptok(p)->kind != TOK_EOF) {
		/* Skip typedef declarations — lcc doesn't register new
		 * typedef names, but the preprocessor passes them through.
		 * Just skip to the next semicolon. */
		if (is_tok(p, "typedef")) {
			while (ptok(p)->kind != TOK_EOF && !is_tok(p, ";"))
				++p->pos;
			if (ptok(p)->kind != TOK_EOF)
				++p->pos;
			continue;
		}
		skip_storage_prefix(p);
		struct Type ret = parse_type(p);
		if (ptok(p)->kind != TOK_ID)
			perr(p, "expected identifier");
		char* name = xstrdup_n(ptok(p)->text, ptok(p)->len);
		int line = ptok(p)->line, col = ptok(p)->col;
		++p->pos;
		if (eat_tok(p, "(")) {
			struct Type params[64];
			struct Symbol* ps[64];
			int n = 0;
			int variadic = 0;
			if (!eat_tok(p, ")")) {
				if (is_tok(p, "void") &&
				    p->tv->v[p->pos + 1].len == 1 &&
				    p->tv->v[p->pos + 1].text[0] == ')') {
					++p->pos;
					expect_tok(p, ")");
				} else {
					for (;;) {
						if (eat_tok(p, "...")) {
							variadic = 1;
							expect_tok(p, ")");
							break;
						}
						if (n >= 64)
							perr(p,
							     "too many "
							     "parameters");
						params[n] = parse_type(p);
						if (ptok(p)->kind == TOK_ID) {
							ps[n] = sym_new(
							    ptok(p)->text,
							    ptok(p)->len);
							++p->pos;
						} else {
							ps[n] =
							    sym_new("__arg", 5);
						}
						ps[n]->type = params[n];
						ps[n]->stack_off = 0;
						++n;
						if (eat_tok(p, ")"))
							break;
						expect_tok(p, ",");
					}
				}
			}
			struct Symbol* fn = get_or_create_function(
			    p, name, u_strlen(name), ret, params, n, variadic);
			if (eat_tok(p, ";")) {
				free(name);
				continue;
			}
			if (fn->defined)
				perr(p, "duplicate function definition");
			fn->defined = 1;
			p->current_return_type = ret;
			parser_push_scope(p);
			{
				int i;
				for (i = 0; i < n; ++i) {
					if (scope_has(
						&p->scopes[p->nscopes - 1],
						ps[i]->name))
						perr(p, "duplicate parameter");
					sv_push(&p->scopes[p->nscopes - 1].syms,
						ps[i]);
				}
			}
			{
				struct Stmt* body = parse_statement(
				    p); /* parse_statement pushes a block scope
					   for the body */
				struct FunctionDef f;
				f.sym = fn;
				f.params = (struct Symbol**)xmalloc(
				    n * sizeof(*f.params));
				if (n && !f.params)
					die_plain("lcc: out of memory");
				if (n)
					mc_memcpy(f.params,
						  ps,
						  n * sizeof(*f.params));
				f.nparams = n;
				f.body = body;
				fv_push(&p->funcs, f);
			}
			parser_pop_scope(p);
			free(name);
		} else {
			struct Symbol* g = sym_new(name, u_strlen(name));
			g->type = ret;
			g->is_function = 0;
			if (sym_find(&p->globals, name))
				perr(p, "duplicate global definition");
			sv_push(&p->globals, g);
			do {
				struct GlobalInit gi;
				gi.sym = g;
				gi.init = 0;
				if (eat_tok(p, "="))
					gi.init = parse_expression(p);
				gv_push(&p->globals_init, gi);
				if (!eat_tok(p, ","))
					break;
				if (ptok(p)->kind != TOK_ID)
					perr(p, "expected variable name");
				name = xstrdup_n(ptok(p)->text, ptok(p)->len);
				++p->pos;
				g = sym_new(name, u_strlen(name));
				g->type = ret;
				g->is_function = 0;
				if (sym_find(&p->globals, name))
					perr(p, "duplicate global definition");
				sv_push(&p->globals, g);
				free(name);
			} while (1);
			expect_tok(p, ";");
			(void)line;
			(void)col;
		}
	}
}

static unsigned int
rv_r(int funct7, int rs2, int rs1, int funct3, int rd, int opcode)
{
	return ((unsigned)funct7 << 25) | ((unsigned)rs2 << 20) |
	       ((unsigned)rs1 << 15) | ((unsigned)funct3 << 12) |
	       ((unsigned)rd << 7) | (unsigned)opcode;
}

static unsigned int rv_i(int imm, int rs1, int funct3, int rd, int opcode)
{
	return ((unsigned)(imm & 0xfff) << 20) | ((unsigned)rs1 << 15) |
	       ((unsigned)funct3 << 12) | ((unsigned)rd << 7) |
	       (unsigned)opcode;
}

static unsigned int rv_s(int imm, int rs2, int rs1, int funct3)
{
	unsigned x = (unsigned)imm;
	return ((x >> 5 & 0x7f) << 25) | ((unsigned)rs2 << 20) |
	       ((unsigned)rs1 << 15) | ((unsigned)funct3 << 12) |
	       ((x & 0x1f) << 7) | 0x23;
}

static unsigned int rv_b(int imm, int rs2, int rs1, int funct3)
{
	unsigned x = (unsigned)imm;
	return (((x >> 12) & 1) << 31) | (((x >> 5) & 0x3f) << 25) |
	       ((unsigned)rs2 << 20) | ((unsigned)rs1 << 15) |
	       ((unsigned)funct3 << 12) | (((x >> 1) & 0xf) << 8) |
	       (((x >> 11) & 1) << 7) | 0x63;
}

static unsigned int rv_u(int imm20, int rd, int opcode)
{
	return ((unsigned)imm20 & 0xfffff000) | ((unsigned)rd << 7) |
	       (unsigned)opcode;
}

static unsigned int rv_j(int imm, int rd)
{
	unsigned x = (unsigned)imm;
	return (((x >> 20) & 1) << 31) | (((x >> 1) & 0x3ff) << 21) |
	       (((x >> 11) & 1) << 20) | (((x >> 12) & 0xff) << 12) |
	       ((unsigned)rd << 7) | 0x6f;
}

static unsigned int rv_addi(int rd, int rs1, int imm)
{
	return rv_i(imm, rs1, 0, rd, 0x13);
}
static unsigned int rv_ld(int rd, int rs1, int imm)
{
	return rv_i(imm, rs1, 3, rd, 0x03);
}
static unsigned int rv_lw(int rd, int rs1, int imm)
{
	return rv_i(imm, rs1, 2, rd, 0x03);
}
static unsigned int rv_lb(int rd, int rs1, int imm)
{
	return rv_i(imm, rs1, 0, rd, 0x03);
}
static unsigned int rv_sd(int rs2, int rs1, int imm)
{
	return rv_s(imm, rs2, rs1, 3);
}
static unsigned int rv_sw(int rs2, int rs1, int imm)
{
	return rv_s(imm, rs2, rs1, 2);
}
static unsigned int rv_sb(int rs2, int rs1, int imm)
{
	return rv_s(imm, rs2, rs1, 0);
}

struct ObjSym {
	char* name;
	int bind;
	int type;
	int section; /* 0 undef, 1 text, 2 data, 3 bss */
	unsigned long long value;
	unsigned long long size;
};

struct ObjSymVec {
	struct ObjSym* v;
	int n;
	int cap;
};

struct Reloc {
	unsigned long long offset;
	unsigned int type;
	int sym;
	long long addend;
};

struct RelocVec {
	struct Reloc* v;
	int n;
	int cap;
};

struct Object {
	struct ByteVec text;
	struct ByteVec data;
	unsigned long long bss_size;
	unsigned long long data_align;
	struct ObjSymVec syms;
	struct RelocVec text_relocs;
	struct RelocVec data_relocs;
};

static void obj_init(struct Object* o)
{
	mc_memset(o, 0, sizeof(*o));
	bv_init(&o->text);
	bv_init(&o->data);
	o->data_align = 1;
}

static int obj_sym_find(struct Object* o, const char* name)
{
	int i;
	for (i = 0; i < o->syms.n; ++i)
		if (u_streq(o->syms.v[i].name, name))
			return i;
	return -1;
}

static int obj_sym_add(struct Object* o,
		       const char* name,
		       int bind,
		       int type,
		       int section,
		       unsigned long long value,
		       unsigned long long size)
{
	int old = obj_sym_find(o, name);
	struct ObjSym* p;
	if (old >= 0) {
		/* A call may create an undefined function symbol before its
		 * later definition is emitted. Upgrade that existing symbol. */
		p = &o->syms.v[old];
		if (section != 0 && p->section == 0) {
			p->bind = bind;
			p->type = type;
			p->section = section;
			p->value = value;
			p->size = size;
		} else if (size != 0 && p->size == 0) {
			p->size = size;
		}
		return old;
	}
	if (o->syms.n == o->syms.cap) {
		int nc = o->syms.cap ? o->syms.cap * 2 : 64;
		p = (struct ObjSym*)realloc(o->syms.v, nc * sizeof(*p));
		if (!p)
			die_plain("lcc: out of memory");
		o->syms.v = p;
		o->syms.cap = nc;
	}
	p = &o->syms.v[o->syms.n];
	mc_memset(p, 0, sizeof(*p));
	p->name = xstrdup_n(name, u_strlen(name));
	p->bind = bind;
	p->type = type;
	p->section = section;
	p->value = value;
	p->size = size;
	return o->syms.n++;
}

static int reloc_add(struct RelocVec* v,
		     unsigned long long off,
		     unsigned int type,
		     int sym,
		     long long addend)
{
	if (v->n == v->cap) {
		int nc = v->cap ? v->cap * 2 : 64;
		struct Reloc* p = (struct Reloc*)realloc(v->v, nc * sizeof(*p));
		if (!p)
			return -1;
		v->v = p;
		v->cap = nc;
	}
	v->v[v->n].offset = off;
	v->v[v->n].type = type;
	v->v[v->n].sym = sym;
	v->v[v->n].addend = addend;
	++v->n;
	return 0;
}

struct CodeGen {
	struct Parser* parser;
	struct Object obj;
	int next_string;
	int next_label;
	struct FunctionDef* fn;
	int return_label;
	unsigned long long frame_size;
	int loop_break[128];
	int loop_continue[128];
	int loop_n;
};

static int new_label(struct CodeGen* g)
{
	return g->next_label++;
}

static void emit32cg(struct CodeGen* g, unsigned int x)
{
	unsigned char b[4];
	wr32(b, x);
	if (bv_append(&g->obj.text, b, 4) < 0)
		die_plain("lcc: out of memory");
}

static unsigned long cg_pc(struct CodeGen* g)
{
	return g->obj.text.n;
}

struct LabelDef {
	int id;
	unsigned long off;
};
struct LabelDefVec {
	struct LabelDef* v;
	int n;
	int cap;
};
struct JumpFix {
	unsigned long off;
	int label;
	int kind;
};
struct JumpFixVec {
	struct JumpFix* v;
	int n;
	int cap;
};

static void ldv_add(struct LabelDefVec* v, int id, unsigned long off)
{
	if (v->n == v->cap) {
		int nc = v->cap ? v->cap * 2 : 64;
		v->v = (struct LabelDef*)realloc(v->v, nc * sizeof(*v->v));
		if (!v->v)
			die_plain("lcc: out of memory");
		v->cap = nc;
	}
	v->v[v->n].id = id;
	v->v[v->n].off = off;
	++v->n;
}
static void
jfv_add(struct JumpFixVec* v, unsigned long off, int label, int kind)
{
	if (v->n == v->cap) {
		int nc = v->cap ? v->cap * 2 : 64;
		v->v = (struct JumpFix*)realloc(v->v, nc * sizeof(*v->v));
		if (!v->v)
			die_plain("lcc: out of memory");
		v->cap = nc;
	}
	v->v[v->n].off = off;
	v->v[v->n].label = label;
	v->v[v->n].kind = kind;
	++v->n;
}

static unsigned long label_off(struct LabelDefVec* v, int id)
{
	int i;
	for (i = 0; i < v->n; ++i)
		if (v->v[i].id == id)
			return v->v[i].off;
	return ~0UL;
}

static void patch_local_jumps(struct CodeGen* g,
			      struct LabelDefVec* labels,
			      struct JumpFixVec* fixes)
{
	int i;
	for (i = 0; i < fixes->n; ++i) {
		unsigned long target = label_off(labels, fixes->v[i].label);
		unsigned long at = fixes->v[i].off;
		long long d = (long long)target - (long long)at;
		unsigned char* p = g->obj.text.p + at;
		unsigned int old = rd32(p), ins;
		if (target == ~0UL)
			die_plain("lcc: unresolved local label");
		if (fixes->v[i].kind == 1)
			ins = rv_j((int)d, 0);
		else {
			int rs1 = (old >> 15) & 31, rs2 = (old >> 20) & 31,
			    funct3 = (old >> 12) & 7;
			ins = rv_b((int)d, rs2, rs1, funct3);
		}
		wr32(p, ins);
	}
}

static void push_reg(struct CodeGen* g, int r)
{
	emit32cg(g, rv_addi(2, 2, -8));
	emit32cg(g, rv_sd(r, 2, 0));
}
static void pop_reg(struct CodeGen* g, int r)
{
	emit32cg(g, rv_ld(r, 2, 0));
	emit32cg(g, rv_addi(2, 2, 8));
}

static void emit_load_sym_addr(struct CodeGen* g, struct Symbol* sym, int rd)
{
	int si = obj_sym_find(&g->obj, sym->name);
	unsigned long at = cg_pc(g);
	emit32cg(g, rv_u(0, rd, 0x37));
	emit32cg(g, rv_addi(rd, rd, 0));
	if (si < 0)
		si = obj_sym_add(
		    &g->obj,
		    sym->name,
		    STB_GLOBAL,
		    sym->is_function ? STT_FUNC : STT_OBJECT,
		    sym->defined ? (sym->is_function ? 1 : sym->section) : 0,
		    sym->value,
		    sym->size);
	if (reloc_add(&g->obj.text_relocs, at, R_RISCV_HI20, si, 0) < 0 ||
	    reloc_add(&g->obj.text_relocs, at + 4, R_RISCV_LO12_I, si, 0) < 0)
		die_plain("lcc: out of memory");
}

static void
emit_load_str_addr(struct CodeGen* g, const char* s, unsigned long n, int rd)
{
	char name[64];
	unsigned long off = g->obj.data.n;
	int si;
	unsigned long at;
	if (bv_append(&g->obj.data, s, n) < 0 ||
	    bv_append(&g->obj.data, "", 1) < 0)
		die_plain("lcc: out of memory");
	{
		char d[32];
		put_u64_dec(d, (unsigned long long)g->next_string);
		mc_copy(name, ".LC");
		mc_cat(name, d);
		++g->next_string;
	}
	si = obj_sym_add(&g->obj, name, STB_LOCAL, STT_OBJECT, 2, off, n + 1);
	at = cg_pc(g);
	emit32cg(g, rv_u(0, rd, 0x37));
	emit32cg(g, rv_addi(rd, rd, 0));
	if (reloc_add(&g->obj.text_relocs, at, R_RISCV_HI20, si, 0) < 0 ||
	    reloc_add(&g->obj.text_relocs, at + 4, R_RISCV_LO12_I, si, 0) < 0)
		die_plain("lcc: out of memory");
}

static void emit_li64(struct CodeGen* g, int rd, unsigned long long bits)
{
	long long x = (long long)bits;
	if (x >= -2048 && x <= 2047) {
		emit32cg(g, rv_addi(rd, 0, (int)x));
		return;
	}
	{
		long long hi = x >> 12;
		long long lo = x - (hi << 12);
		emit_li64(g, rd, (unsigned long long)hi);
		emit32cg(g, rv_i(12, rd, 1, rd, 0x13));
		if (lo != 0)
			emit32cg(g, rv_addi(rd, rd, (int)lo));
	}
}

static void gen_expr(struct CodeGen* g,
		     struct Expr* e,
		     struct LabelDefVec* labels,
		     struct JumpFixVec* fixes);

static int local_offset(struct CodeGen* g, struct Symbol* s)
{
	(void)g;
	return (int)s->stack_off;
}

static void gen_laddr(struct CodeGen* g,
		      struct Expr* e,
		      struct LabelDefVec* labels,
		      struct JumpFixVec* fixes)
{
	if (e->kind == EX_VAR) {
		if (e->sym->is_function)
			die_plain("lcc: function is not an lvalue");
		if (e->sym->stack_off)
			emit32cg(g, rv_addi(10, 8, local_offset(g, e->sym)));
		else
			emit_load_sym_addr(g, e->sym, 10);
		return;
	}
	if (e->kind == EX_INDEX) {
		gen_expr(g, e->a, labels, fixes);
		push_reg(g, 10);
		gen_expr(g, e->b, labels, fixes);
		pop_reg(g, 5);
		{
			int sz = (int)type_size(e->type), sh = 0;
			if (sz == 8)
				sh = 3;
			else if (sz == 4)
				sh = 2;
			else if (sz == 2)
				sh = 1;
			if (sh)
				emit32cg(g, rv_i(sh, 10, 1, 10, 0x13));
		}
		emit32cg(g, rv_r(0, 10, 5, 0, 10, 0x33));
		return;
	}
	die_plain("lcc: expression is not an lvalue");
}

static void gen_binary(struct CodeGen* g,
		       struct Expr* e,
		       struct LabelDefVec* labels,
		       struct JumpFixVec* fixes)
{
	if (u_streq(e->op, "&&")) {
		int lf = new_label(g), le = new_label(g);
		gen_expr(g, e->a, labels, fixes);
		jfv_add(fixes, cg_pc(g), lf, 2);
		emit32cg(g, rv_b(0, 0, 10, 0));
		gen_expr(g, e->b, labels, fixes);
		jfv_add(fixes, cg_pc(g), lf, 2);
		emit32cg(g, rv_b(0, 0, 10, 0));
		emit_li64(g, 10, 1);
		jfv_add(fixes, cg_pc(g), le, 1);
		emit32cg(g, rv_j(0, 0));
		ldv_add(labels, lf, cg_pc(g));
		emit_li64(g, 10, 0);
		ldv_add(labels, le, cg_pc(g));
		return;
	}
	if (u_streq(e->op, "||")) {
		int lt = new_label(g), le = new_label(g);
		gen_expr(g, e->a, labels, fixes);
		jfv_add(fixes, cg_pc(g), lt, 3);
		emit32cg(g, rv_b(0, 0, 10, 1));
		gen_expr(g, e->b, labels, fixes);
		jfv_add(fixes, cg_pc(g), lt, 3);
		emit32cg(g, rv_b(0, 0, 10, 1));
		emit_li64(g, 10, 0);
		jfv_add(fixes, cg_pc(g), le, 1);
		emit32cg(g, rv_j(0, 0));
		ldv_add(labels, lt, cg_pc(g));
		emit_li64(g, 10, 1);
		ldv_add(labels, le, cg_pc(g));
		return;
	}
	gen_expr(g, e->a, labels, fixes);
	push_reg(g, 10);
	gen_expr(g, e->b, labels, fixes);
	pop_reg(g, 5);
	if (u_streq(e->op, "+") || u_streq(e->op, "-")) {
		int scale = 0;
		if (e->a->type.ptr)
			scale = (int)type_size(
			    (struct Type){e->a->type.base, e->a->type.ptr - 1});
		else if (e->b->type.ptr)
			scale = (int)type_size(
			    (struct Type){e->b->type.base, e->b->type.ptr - 1});
		if (e->a->type.ptr && !e->b->type.ptr && scale > 1) {
			int sh = scale == 8   ? 3
				 : scale == 4 ? 2
				 : scale == 2 ? 1
					      : 0;
			if (sh)
				emit32cg(g, rv_i(sh, 10, 1, 10, 0x13));
		}
		if (e->b->type.ptr && !e->a->type.ptr && scale > 1) {
			int sh = scale == 8   ? 3
				 : scale == 4 ? 2
				 : scale == 2 ? 1
					      : 0;
			if (sh)
				emit32cg(g, rv_i(sh, 5, 1, 5, 0x13));
		}
		if (u_streq(e->op, "+"))
			emit32cg(g, rv_r(0, 10, 5, 0, 10, 0x33));
		else
			emit32cg(g, rv_r(0x20, 10, 5, 0, 10, 0x33));
	} else if (u_streq(e->op, "*"))
		emit32cg(g, rv_r(1, 10, 5, 0, 10, 0x33));
	else if (u_streq(e->op, "/"))
		emit32cg(g, rv_r(1, 10, 5, 4, 10, 0x33));
	else if (u_streq(e->op, "%"))
		emit32cg(g, rv_r(1, 10, 5, 6, 10, 0x33));
	else if (u_streq(e->op, "&"))
		emit32cg(g, rv_r(0, 10, 5, 7, 10, 0x33));
	else if (u_streq(e->op, "|"))
		emit32cg(g, rv_r(0, 10, 5, 6, 10, 0x33));
	else if (u_streq(e->op, "^"))
		emit32cg(g, rv_r(0, 10, 5, 4, 10, 0x33));
	else if (u_streq(e->op, "<<"))
		emit32cg(g, rv_r(0, 10, 5, 1, 10, 0x33));
	else if (u_streq(e->op, ">>"))
		emit32cg(g, rv_r(0, 10, 5, 5, 10, 0x33));
	else if (u_streq(e->op, "<"))
		emit32cg(g, rv_r(0, 10, 5, 2, 10, 0x33));
	else if (u_streq(e->op, ">"))
		emit32cg(g, rv_r(0, 5, 10, 2, 10, 0x33));
	else if (u_streq(e->op, "<=")) {
		emit32cg(g, rv_r(0, 5, 10, 2, 10, 0x33));
		emit32cg(g, rv_i(1, 10, 4, 10, 0x13));
	} else if (u_streq(e->op, ">=")) {
		emit32cg(g, rv_r(0, 10, 5, 2, 10, 0x33));
		emit32cg(g, rv_i(1, 10, 4, 10, 0x13));
	} else if (u_streq(e->op, "==")) {
		emit32cg(g, rv_r(0, 10, 5, 4, 10, 0x33));
		emit32cg(g, rv_i(1, 10, 3, 10, 0x13));
	} else if (u_streq(e->op, "!=")) {
		emit32cg(g, rv_r(0, 10, 5, 4, 10, 0x33));
		emit32cg(g, rv_r(0, 10, 0, 3, 10, 0x33));
	} else
		die_plain("lcc: unsupported binary operator");
}

static void gen_expr(struct CodeGen* g,
		     struct Expr* e,
		     struct LabelDefVec* labels,
		     struct JumpFixVec* fixes)
{
	int i;
	switch (e->kind) {
	case EX_INT:
	case EX_CHAR:
		emit_li64(g, 10, (long long)e->value);
		return;
	case EX_STRING:
		emit_load_str_addr(g, e->text, e->text_len, 10);
		return;
	case EX_VAR:
		if (e->sym->is_function)
			die_plain("lcc: bare function value is not supported");
		if (e->sym->stack_off) {
			int off = (int)e->sym->stack_off;
			if (type_size(e->sym->type) == 8)
				emit32cg(g, rv_ld(10, 8, off));
			else if (type_size(e->sym->type) == 4)
				emit32cg(g, rv_lw(10, 8, off));
			else
				emit32cg(g, rv_lb(10, 8, off));
		} else {
			emit_load_sym_addr(g, e->sym, 5);
			if (type_size(e->sym->type) == 8)
				emit32cg(g, rv_ld(10, 5, 0));
			else if (type_size(e->sym->type) == 4)
				emit32cg(g, rv_lw(10, 5, 0));
			else
				emit32cg(g, rv_lb(10, 5, 0));
		}
		return;
	case EX_UNARY:
		if (u_streq(e->op, "&")) {
			gen_laddr(g, e->a, labels, fixes);
			return;
		}
		if (u_streq(e->op, "*")) {
			gen_expr(g, e->a, labels, fixes);
			if (type_size(e->type) == 8)
				emit32cg(g, rv_ld(10, 10, 0));
			else if (type_size(e->type) == 4)
				emit32cg(g, rv_lw(10, 10, 0));
			else
				emit32cg(g, rv_lb(10, 10, 0));
			return;
		}
		if (u_streq(e->op, "+")) {
			gen_expr(g, e->a, labels, fixes);
			return;
		}
		if (u_streq(e->op, "-")) {
			gen_expr(g, e->a, labels, fixes);
			emit32cg(g, rv_r(0x20, 10, 0, 0, 10, 0x33));
			return;
		}
		if (u_streq(e->op, "!")) {
			gen_expr(g, e->a, labels, fixes);
			emit32cg(g, rv_i(1, 10, 3, 10, 0x13));
			return;
		}
		if (u_streq(e->op, "~")) {
			gen_expr(g, e->a, labels, fixes);
			emit32cg(g, rv_i(-1, 10, 4, 10, 0x13));
			return;
		}
		if (u_streq(e->op, "++") || u_streq(e->op, "--") ||
		    u_streq(e->op, "post++") || u_streq(e->op, "post--")) {
			int post = (e->op[0] == 'p');
			int delta =
			    (u_streq(e->op, "--") || u_streq(e->op, "post--"))
				? -1
				: 1;
			int addr = 5, old = 6;
			int sz = (int)type_size(e->a->type);
			if (e->a->type.ptr) {
				struct Type et = {e->a->type.base,
						  e->a->type.ptr - 1};
				delta *= (int)type_size(et);
			}
			gen_laddr(g, e->a, labels, fixes);
			if (sz == 8)
				emit32cg(g, rv_ld(old, addr, 0));
			else if (sz == 4)
				emit32cg(g, rv_lw(old, addr, 0));
			else
				emit32cg(g, rv_lb(old, addr, 0));
			if (post)
				push_reg(g, old);
			emit32cg(g, rv_addi(old, old, delta));
			if (sz == 8)
				emit32cg(g, rv_sd(old, addr, 0));
			else if (sz == 4)
				emit32cg(g, rv_sw(old, addr, 0));
			else
				emit32cg(g, rv_sb(old, addr, 0));
			if (post)
				pop_reg(g, 10);
			else
				emit32cg(g, rv_addi(10, old, 0));
			return;
		}
		die_plain("lcc: unsupported unary operator");
		return;
	case EX_BINARY:
		gen_binary(g, e, labels, fixes);
		return;
	case EX_INDEX:
		gen_laddr(g, e, labels, fixes);
		if (type_size(e->type) == 8)
			emit32cg(g, rv_ld(10, 10, 0));
		else if (type_size(e->type) == 4)
			emit32cg(g, rv_lw(10, 10, 0));
		else
			emit32cg(g, rv_lb(10, 10, 0));
		return;
	case EX_ASSIGN: {
		gen_laddr(g, e->a, labels, fixes);
		push_reg(g, 10);
		if (!u_streq(e->op, "=")) {
			emit32cg(g, rv_ld(6, 2, 0));
			push_reg(g, 6);
		}
		gen_expr(g, e->b, labels, fixes);
		if (!u_streq(e->op, "=")) {
			pop_reg(g, 6);
			pop_reg(g, 5);
			if (u_streq(e->op, "+="))
				emit32cg(g, rv_r(0, 10, 6, 0, 10, 0x33));
			else if (u_streq(e->op, "-="))
				emit32cg(g, rv_r(0x20, 10, 6, 0, 10, 0x33));
			else if (u_streq(e->op, "*="))
				emit32cg(g, rv_r(1, 10, 6, 0, 10, 0x33));
			else if (u_streq(e->op, "/="))
				emit32cg(g, rv_r(1, 10, 6, 4, 10, 0x33));
			else if (u_streq(e->op, "%="))
				emit32cg(g, rv_r(1, 10, 6, 6, 10, 0x33));
			else if (u_streq(e->op, "&="))
				emit32cg(g, rv_r(0, 10, 6, 7, 10, 0x33));
			else if (u_streq(e->op, "|="))
				emit32cg(g, rv_r(0, 10, 6, 6, 10, 0x33));
			else if (u_streq(e->op, "^="))
				emit32cg(g, rv_r(0, 10, 6, 4, 10, 0x33));
			else
				die_plain(
				    "lcc: unsupported compound assignment");
			if (type_size(e->a->type) == 8)
				emit32cg(g, rv_sd(10, 5, 0));
			else if (type_size(e->a->type) == 4)
				emit32cg(g, rv_sw(10, 5, 0));
			else
				emit32cg(g, rv_sb(10, 5, 0));
		} else {
			pop_reg(g, 5);
			if (type_size(e->a->type) == 8)
				emit32cg(g, rv_sd(10, 5, 0));
			else if (type_size(e->a->type) == 4)
				emit32cg(g, rv_sw(10, 5, 0));
			else
				emit32cg(g, rv_sb(10, 5, 0));
		}
		return;
	}
	case EX_CALL:
		if (e->args.n > MAX_ARGS)
			die_plain(
			    "lcc: more than 8 arguments are not supported");
		for (i = e->args.n - 1; i >= 0; --i) {
			gen_expr(g, e->args.v[i], labels, fixes);
			push_reg(g, 10);
		}
		for (i = 0; i < e->args.n; ++i)
			pop_reg(g, 10 + i);
		{
			unsigned long at = cg_pc(g);
			emit32cg(g, rv_u(0, 1, 0x17));
			emit32cg(g, rv_i(0, 1, 0, 1, 0x67));
			int si = obj_sym_find(&g->obj, e->sym->name);
			if (si < 0)
				si = obj_sym_add(&g->obj,
						 e->sym->name,
						 STB_GLOBAL,
						 STT_FUNC,
						 e->sym->defined ? 1 : 0,
						 0,
						 0);
			if (reloc_add(
				&g->obj.text_relocs, at, R_RISCV_CALL, si, 0) <
			    0)
				die_plain("lcc: out of memory");
		}
		return;
	case EX_COND: {
		int ln = new_label(g), le = new_label(g);
		gen_expr(g, e->a, labels, fixes);
		jfv_add(fixes, cg_pc(g), ln, 2);
		emit32cg(g, rv_b(0, 0, 10, 0));
		gen_expr(g, e->b, labels, fixes);
		jfv_add(fixes, cg_pc(g), le, 1);
		emit32cg(g, rv_j(0, 0));
		ldv_add(labels, ln, cg_pc(g));
		gen_expr(g, e->c, labels, fixes);
		ldv_add(labels, le, cg_pc(g));
		return;
	}
	case EX_SIZEOF:
		emit_li64(g, 10, (long long)e->value);
		return;
	}
}

static void assign_local_offsets_stmt(struct Stmt* s, int* slot)
{
	int i;
	if (!s)
		return;
	if (s->kind == ST_DECL) {
		s->sym->stack_off = -24 - (*slot) * 8;
		++*slot;
		return;
	}
	if (s->kind == ST_BLOCK) {
		for (i = 0; i < s->body.n; ++i)
			assign_local_offsets_stmt(s->body.v[i], slot);
		return;
	}
	if (s->kind == ST_IF) {
		assign_local_offsets_stmt(s->s1, slot);
		assign_local_offsets_stmt(s->s2, slot);
		return;
	}
	if (s->kind == ST_WHILE) {
		assign_local_offsets_stmt(s->s1, slot);
		return;
	}
	if (s->kind == ST_FOR) {
		assign_local_offsets_stmt(s->s1, slot);
		assign_local_offsets_stmt(s->s2, slot);
	}
}

static void assign_param_offsets(struct FunctionDef* f)
{
	int i;
	for (i = 0; i < f->nparams; ++i)
		f->params[i]->stack_off = -24 - i * 8;
}

static void gen_stmt(struct CodeGen* g,
		     struct Stmt* s,
		     struct LabelDefVec* labels,
		     struct JumpFixVec* fixes)
{
	int i;
	switch (s->kind) {
	case ST_BLOCK:
		for (i = 0; i < s->body.n; ++i)
			gen_stmt(g, s->body.v[i], labels, fixes);
		break;
	case ST_DECL:
		if (s->e) {
			gen_expr(g, s->e, labels, fixes);
			if (type_size(s->sym->type) == 8)
				emit32cg(g,
					 rv_sd(10, 8, (int)s->sym->stack_off));
			else if (type_size(s->sym->type) == 4)
				emit32cg(g,
					 rv_sw(10, 8, (int)s->sym->stack_off));
			else
				emit32cg(g,
					 rv_sb(10, 8, (int)s->sym->stack_off));
		}
		break;
	case ST_EXPR:
		gen_expr(g, s->e, labels, fixes);
		break;
	case ST_IF: {
		int ln = new_label(g), le = new_label(g);
		gen_expr(g, s->e, labels, fixes);
		jfv_add(fixes, cg_pc(g), ln, 2);
		emit32cg(g, rv_b(0, 0, 10, 0));
		gen_stmt(g, s->s1, labels, fixes);
		if (s->s2) {
			jfv_add(fixes, cg_pc(g), le, 1);
			emit32cg(g, rv_j(0, 0));
			ldv_add(labels, ln, cg_pc(g));
			gen_stmt(g, s->s2, labels, fixes);
			ldv_add(labels, le, cg_pc(g));
		} else
			ldv_add(labels, ln, cg_pc(g));
		break;
	}
	case ST_WHILE: {
		int lc = new_label(g), le = new_label(g);
		ldv_add(labels, lc, cg_pc(g));
		gen_expr(g, s->e, labels, fixes);
		jfv_add(fixes, cg_pc(g), le, 2);
		emit32cg(g, rv_b(0, 0, 10, 0));
		g->loop_break[g->loop_n] = le;
		g->loop_continue[g->loop_n] = lc;
		++g->loop_n;
		gen_stmt(g, s->s1, labels, fixes);
		--g->loop_n;
		jfv_add(fixes, cg_pc(g), lc, 1);
		emit32cg(g, rv_j(0, 0));
		ldv_add(labels, le, cg_pc(g));
		break;
	}
	case ST_FOR: {
		int lc = new_label(g), ls = new_label(g), le = new_label(g);
		if (s->s1)
			gen_stmt(g, s->s1, labels, fixes);
		ldv_add(labels, lc, cg_pc(g));
		if (s->e) {
			gen_expr(g, s->e, labels, fixes);
			jfv_add(fixes, cg_pc(g), le, 2);
			emit32cg(g, rv_b(0, 0, 10, 0));
		}
		g->loop_break[g->loop_n] = le;
		g->loop_continue[g->loop_n] = ls;
		++g->loop_n;
		gen_stmt(g, s->s2, labels, fixes);
		--g->loop_n;
		ldv_add(labels, ls, cg_pc(g));
		if (s->e2)
			gen_expr(g, s->e2, labels, fixes);
		jfv_add(fixes, cg_pc(g), lc, 1);
		emit32cg(g, rv_j(0, 0));
		ldv_add(labels, le, cg_pc(g));
		break;
	}
	case ST_RETURN:
		if (s->e)
			gen_expr(g, s->e, labels, fixes);
		else
			emit_li64(g, 10, 0);
		jfv_add(fixes, cg_pc(g), g->return_label, 1);
		emit32cg(g, rv_j(0, 0));
		break;
	case ST_BREAK:
		jfv_add(fixes,
			cg_pc(g),
			g->loop_n ? g->loop_break[g->loop_n - 1] : -1,
			1);
		emit32cg(g, rv_j(0, 0));
		break;
	case ST_CONTINUE:
		jfv_add(fixes,
			cg_pc(g),
			g->loop_n ? g->loop_continue[g->loop_n - 1] : -1,
			1);
		emit32cg(g, rv_j(0, 0));
		break;
	}
}

static void generate_object(struct CodeGen* g)
{
	int i;
	obj_init(&g->obj);
	/* Globals */
	for (i = 0; i < g->parser->globals_init.n; ++i) {
		struct GlobalInit* gi = &g->parser->globals_init.v[i];
		struct Symbol* s = gi->sym;
		unsigned long ts = type_size(s->type);
		if (!gi->init) {
			unsigned long ba = ts >= 8 ? 8 : (ts >= 4 ? 4 : 1);
			g->obj.bss_size = align_up(g->obj.bss_size, ba);
			s->section = 3;
			s->value = g->obj.bss_size;
			s->size = ts;
			g->obj.bss_size += ts;
			obj_sym_add(&g->obj,
				    s->name,
				    STB_GLOBAL,
				    STT_OBJECT,
				    3,
				    s->value,
				    s->size);
			continue;
		}
		{
			unsigned long da = ts >= 8 ? 8 : (ts >= 4 ? 4 : 1);
			if (da > g->obj.data_align)
				g->obj.data_align = da;
			bv_align(&g->obj.data, da);
		}
		s->section = 2;
		s->value = g->obj.data.n;
		s->size = ts;
		obj_sym_add(&g->obj,
			    s->name,
			    STB_GLOBAL,
			    STT_OBJECT,
			    2,
			    s->value,
			    s->size);
		if (gi->init->kind == EX_INT || gi->init->kind == EX_CHAR) {
			if (ts == 1) {
				unsigned char b =
				    (unsigned char)gi->init->value;
				bv_append(&g->obj.data, &b, 1);
			} else if (ts == 4)
				bv_u32(&g->obj.data,
				       (unsigned int)gi->init->value);
			else
				bv_u64(&g->obj.data, gi->init->value);
		} else if (gi->init->kind == EX_STRING && s->type.ptr) {
			unsigned long ptr_off = g->obj.data.n, str_off;
			int si;
			char name[64], d[32];
			bv_u64(&g->obj.data, 0);
			str_off = g->obj.data.n;
			bv_append(
			    &g->obj.data, gi->init->text, gi->init->text_len);
			bv_append(&g->obj.data, "", 1);
			put_u64_dec(d, (unsigned long long)g->next_string);
			mc_copy(name, ".LCG");
			mc_cat(name, d);
			++g->next_string;
			si = obj_sym_add(&g->obj,
					 name,
					 STB_LOCAL,
					 STT_OBJECT,
					 2,
					 str_off,
					 gi->init->text_len + 1);
			reloc_add(
			    &g->obj.data_relocs, ptr_off, R_RISCV_64, si, 0);
		} else
			die_plain("lcc: unsupported global initializer");
	}
	for (i = 0; i < g->parser->funcs.n; ++i) {
		struct FunctionDef* f = &g->parser->funcs.v[i];
		struct LabelDefVec labels = {0};
		struct JumpFixVec fixes = {0};
		int slots = f->nparams;
		assign_param_offsets(f);
		assign_local_offsets_stmt(f->body, &slots);
		g->frame_size =
		    align_up(16 + (unsigned long long)slots * 8, 16);
		f->sym->section = 1;
		f->sym->value = g->obj.text.n;
		obj_sym_add(&g->obj,
			    f->sym->name,
			    STB_GLOBAL,
			    STT_FUNC,
			    1,
			    f->sym->value,
			    0);
		emit32cg(g, rv_addi(2, 2, -(int)g->frame_size));
		emit32cg(g, rv_sd(1, 2, (int)g->frame_size - 8));
		emit32cg(g, rv_sd(8, 2, (int)g->frame_size - 16));
		emit32cg(g, rv_addi(8, 2, (int)g->frame_size));
		{
			int a;
			for (a = 0; a < f->nparams; ++a)
				emit32cg(g,
					 rv_sd(10 + a,
					       8,
					       (int)f->params[a]->stack_off));
		}
		g->fn = f;
		g->return_label = new_label(g);
		g->loop_n = 0;
		gen_stmt(g, f->body, &labels, &fixes);
		emit_li64(g, 10, 0);
		ldv_add(&labels, g->return_label, g->obj.text.n);
		emit32cg(g, rv_addi(2, 8, -(int)g->frame_size));
		emit32cg(g, rv_ld(8, 2, (int)g->frame_size - 16));
		emit32cg(g, rv_ld(1, 2, (int)g->frame_size - 8));
		emit32cg(g, rv_addi(2, 2, (int)g->frame_size));
		emit32cg(g, rv_i(0, 1, 0, 0, 0x67));
		{
			int osi = obj_sym_find(&g->obj, f->sym->name);
			if (osi >= 0)
				g->obj.syms.v[osi].size =
				    g->obj.text.n - f->sym->value;
		}
		patch_local_jumps(g, &labels, &fixes);
		free(labels.v);
		free(fixes.v);
	}
}

static unsigned int
add_str(char** buf, unsigned long* n, unsigned long* cap, const char* s)
{
	unsigned long len = u_strlen(s), off = *n;
	char* p;
	if (*cap < *n + len + 1) {
		unsigned long nc = *cap ? *cap * 2 : 128;
		while (nc < *n + len + 1)
			nc *= 2;
		p = (char*)realloc(*buf, nc);
		if (!p)
			return 0;
		*buf = p;
		*cap = nc;
	}
	mc_memcpy(*buf + *n, s, len);
	(*buf)[*n + len] = 0;
	*n += len + 1;
	return (unsigned int)off;
}

static unsigned char* build_rela_bytes(struct RelocVec* r, int* remap)
{
	unsigned char* p =
	    (unsigned char*)xcalloc(r->n, sizeof(struct Elf64_Rela));
	int i;
	struct Elf64_Rela* a = (struct Elf64_Rela*)p;
	if (!p)
		return 0;
	for (i = 0; i < r->n; ++i) {
		a[i].r_offset = r->v[i].offset;
		a[i].r_info =
		    r_info((unsigned)remap[r->v[i].sym], r->v[i].type);
		a[i].r_addend = r->v[i].addend;
	}
	return p;
}

static int object_to_elf(struct Object* o, struct ByteVec* out)
{
	int i, j, nlocal = 0;
	int* order = (int*)xmalloc(o->syms.n * sizeof(int));
	int* remap = (int*)xmalloc(o->syms.n * sizeof(int));
	char* str = (char*)xmalloc(1);
	unsigned long strn = 1, strcap = 1;
	int sec_text = 1, sec_data = 2, sec_bss = 3, sec_sym = 4, sec_str = 5,
	    sec_rt = 6, sec_rd = 7, sec_shstr = 8;
	unsigned long shstrn = 1, shstrcap = 1;
	char* shstr = (char*)xmalloc(1);
	struct Elf64_Shdr sh[9];
	struct ByteVec sd[9];
	struct Elf64_Ehdr eh;
	int first_global, idx = 0;
	unsigned long off;
	int sym_start;
	mc_memset(sh, 0, sizeof(sh));
	for (i = 0; i < 9; ++i)
		bv_init(&sd[i]);
	if (!order || !remap || !str || !shstr)
		return -1;
	str[0] = 0;
	shstr[0] = 0;
	for (i = 0; i < o->syms.n; ++i)
		if (o->syms.v[i].bind == STB_LOCAL)
			order[nlocal++] = i;
	for (i = 0; i < o->syms.n; ++i)
		if (o->syms.v[i].bind != STB_LOCAL)
			order[nlocal++] = i;
	first_global = 1;
	for (i = 0; i < nlocal; ++i) {
		if (o->syms.v[order[i]].bind != STB_LOCAL) {
			first_global = i + 1;
			break;
		}
	}
	{
		int newi = 1;
		for (i = 0; i < nlocal; ++i) {
			remap[order[i]] = newi++;
			add_str(&str, &strn, &strcap, o->syms.v[order[i]].name);
		}
	}
	bv_append(&sd[1], o->text.p, o->text.n);
	bv_append(&sd[2], o->data.p, o->data.n);
	{
		struct Elf64_Sym nullsym;
		mc_memset(&nullsym, 0, sizeof(nullsym));
		bv_append(&sd[4], &nullsym, sizeof(nullsym));
	}
	sh[1].sh_type = SHT_PROGBITS;
	sh[1].sh_flags = SHF_ALLOC | SHF_EXECINSTR;
	sh[1].sh_addralign = 4;
	sh[2].sh_type = SHT_PROGBITS;
	sh[2].sh_flags = SHF_ALLOC | SHF_WRITE;
	sh[2].sh_addralign = o->data_align ? o->data_align : 1;
	sh[3].sh_type = SHT_NOBITS;
	sh[3].sh_flags = SHF_ALLOC | SHF_WRITE;
	sh[3].sh_addralign = 8;
	sh[3].sh_size = o->bss_size;
	for (i = 0; i < nlocal; ++i) {
		struct Elf64_Sym s;
		struct ObjSym* x = &o->syms.v[order[i]];
		mc_memset(
		    &s, 0, sizeof(s)); /* recover string offset by scanning */
		{
			unsigned long q = 1;
			int k;
			for (k = 0; k < i; ++k) {
				q += u_strlen(o->syms.v[order[k]].name) + 1;
			}
			s.st_name = (unsigned int)q;
		}
		s.st_info = (unsigned char)ST_INFO(x->bind, x->type);
		s.st_shndx = (unsigned short)(x->section == 1	? 1
					      : x->section == 2 ? 2
					      : x->section == 3 ? 3
								: SHN_UNDEF);
		s.st_value = x->value;
		s.st_size = x->size;
		bv_append(&sd[4], &s, sizeof(s));
	}
	sh[4].sh_type = SHT_SYMTAB;
	sh[4].sh_addralign = 8;
	sh[4].sh_entsize = sizeof(struct Elf64_Sym);
	sh[4].sh_link = 5;
	sh[4].sh_info = first_global;
	bv_append(&sd[5], str, strn);
	sh[5].sh_type = SHT_STRTAB;
	sh[5].sh_addralign = 1;
	{
		unsigned char* rtext = build_rela_bytes(&o->text_relocs, remap);
		unsigned char* rdata = build_rela_bytes(&o->data_relocs, remap);
		if (o->text_relocs.n && !rtext)
			return -1;
		if (o->data_relocs.n && !rdata)
			return -1;
		if (rtext)
			bv_append(&sd[6],
				  rtext,
				  o->text_relocs.n * sizeof(struct Elf64_Rela));
		if (rdata)
			bv_append(&sd[7],
				  rdata,
				  o->data_relocs.n * sizeof(struct Elf64_Rela));
		free(rtext);
		free(rdata);
	}
	sh[6].sh_type = SHT_RELA;
	sh[6].sh_addralign = 8;
	sh[6].sh_entsize = sizeof(struct Elf64_Rela);
	sh[6].sh_link = 4;
	sh[6].sh_info = 1;
	sh[7].sh_type = SHT_RELA;
	sh[7].sh_addralign = 8;
	sh[7].sh_entsize = sizeof(struct Elf64_Rela);
	sh[7].sh_link = 4;
	sh[7].sh_info = 2;
	{
		const char* names[] = {"",
				       ".text",
				       ".data",
				       ".bss",
				       ".symtab",
				       ".strtab",
				       ".rela.text",
				       ".rela.data",
				       ".shstrtab"};
		for (i = 0; i < 9; ++i) {
			unsigned long len = u_strlen(names[i]);
			if (shstrcap < shstrn + len + 1) {
				unsigned long nc =
				    shstrcap ? shstrcap * 2 : 128;
				while (nc < shstrn + len + 1)
					nc *= 2;
				shstr = (char*)realloc(shstr, nc);
				shstrcap = nc;
			}
			mc_memcpy(shstr + shstrn, names[i], len + 1);
			sh[i].sh_name = (unsigned int)shstrn;
			shstrn += len + 1;
		}
	}
	bv_append(&sd[8], shstr, shstrn);
	sh[8].sh_type = SHT_STRTAB;
	sh[8].sh_addralign = 1;
	mc_memset(&eh, 0, sizeof(eh));
	bv_init(out);
	bv_resize(out, sizeof(eh));
	off = sizeof(eh);
	for (i = 1; i < 9; ++i) {
		off =
		    align_up(off, sh[i].sh_addralign ? sh[i].sh_addralign : 1);
		sh[i].sh_offset = off;
		if (sh[i].sh_type == SHT_NOBITS) {
			off += sh[i].sh_size;
		} else {
			sh[i].sh_size = sd[i].n;
			bv_resize(out, off + sd[i].n);
			if (sd[i].n)
				mc_memcpy(out->p + off, sd[i].p, sd[i].n);
			off += sd[i].n;
		}
	}
	off = align_up(off, 8);
	eh.e_ident[0] = 0x7f;
	eh.e_ident[1] = 'E';
	eh.e_ident[2] = 'L';
	eh.e_ident[3] = 'F';
	eh.e_ident[4] = ELFCLASS64;
	eh.e_ident[5] = ELFDATA2LSB;
	eh.e_ident[6] = 1;
	eh.e_type = ET_REL;
	eh.e_machine = EM_RISCV;
	eh.e_version = 1;
	eh.e_ehsize = sizeof(eh);
	eh.e_shoff = off;
	eh.e_shentsize = sizeof(struct Elf64_Shdr);
	eh.e_shnum = 9;
	eh.e_shstrndx = 8;
	bv_resize(out, off + 9 * sizeof(struct Elf64_Shdr));
	mc_memcpy(out->p, &eh, sizeof(eh));
	mc_memcpy(out->p + off, sh, sizeof(sh));
	free(order);
	free(remap);
	free(str);
	free(shstr);
	for (i = 0; i < 9; ++i)
		free(sd[i].p);
	(void)sec_text;
	(void)sec_data;
	(void)sec_bss;
	(void)sec_sym;
	(void)sec_str;
	(void)sec_rt;
	(void)sec_rd;
	(void)sec_shstr;
	(void)sym_start;
	(void)idx;
	(void)j;
	return 0;
}

struct LinkObj {
	unsigned char* file;
	unsigned long size;
	struct Elf64_Ehdr eh;
	struct Elf64_Shdr* sh;
	char** names;
	unsigned long long* sec_addr;
	unsigned char* sec_kind; /* 0 ignored, 1 text, 2 data, 3 bss */
	int sym_sec;
	int str_sec;
};

static const char* section_name(struct LinkObj* o, int i)
{
	return o->names[i];
}

static int starts_with(const char* s, const char* prefix)
{
	unsigned long n = u_strlen(prefix);
	return !u_strncmp_n(s, prefix, n);
}

static int
linkobj_parse(struct LinkObj* o, unsigned char* file, unsigned long size)
{
	int i;
	const char* shnames;
	unsigned long shstr_off, shstr_size;

	mc_memset(o, 0, sizeof(*o));
	o->file = file;
	o->size = size;
	o->sym_sec = -1;
	o->str_sec = -1;

	if (size < sizeof(struct Elf64_Ehdr))
		return -1;
	mc_memcpy(&o->eh, file, sizeof(o->eh));

	if (o->eh.e_ident[0] != 0x7f || o->eh.e_ident[1] != 'E' ||
	    o->eh.e_ident[2] != 'L' || o->eh.e_ident[3] != 'F' ||
	    o->eh.e_ident[4] != ELFCLASS64 || o->eh.e_ident[5] != ELFDATA2LSB ||
	    o->eh.e_machine != EM_RISCV || o->eh.e_type != ET_REL)
		return -1;

	if (o->eh.e_shstrndx >= o->eh.e_shnum)
		return -1;
	if (o->eh.e_shoff > size ||
	    o->eh.e_shnum > (size - o->eh.e_shoff) / sizeof(struct Elf64_Shdr))
		return -1;

	o->sh = (struct Elf64_Shdr*)(file + o->eh.e_shoff);

	shstr_off = o->sh[o->eh.e_shstrndx].sh_offset;
	shstr_size = o->sh[o->eh.e_shstrndx].sh_size;
	if (shstr_off > size || shstr_size > size - shstr_off)
		return -1;
	shnames = (const char*)file + shstr_off;

	o->names = (char**)xcalloc(o->eh.e_shnum, sizeof(char*));
	o->sec_addr = (unsigned long long*)xcalloc(o->eh.e_shnum,
						   sizeof(unsigned long long));
	o->sec_kind = (unsigned char*)xcalloc(o->eh.e_shnum, 1);
	if (!o->names || !o->sec_addr || !o->sec_kind)
		return -1;

	for (i = 0; i < o->eh.e_shnum; ++i) {
		unsigned long noff = o->sh[i].sh_name;
		const char* name;
		if (noff >= shstr_size)
			return -1;
		name = shnames + noff;
		o->names[i] = (char*)name;

		if (u_streq(name, ".symtab"))
			o->sym_sec = i;
		if (u_streq(name, ".strtab"))
			o->str_sec = i;

		/* 丢弃异常展开 / 调试 / 注解元数据。.eh_frame 带 SHF_ALLOC，
		 * 但 freestanding 运行时从不做 DWARF 展开，其 SET/PCREL/ADD
		 * 等重定位本链接器也无法处理；直接忽略整段（连同它的 rela
		 * 段）。 debug、comment、riscv.attributes、note
		 * 等段也不参与加载。 */
		if (starts_with(name, ".eh_frame") ||
		    starts_with(name, ".debug") ||
		    starts_with(name, ".comment") ||
		    starts_with(name, ".riscv") || starts_with(name, ".note") ||
		    starts_with(name, ".zdebug"))
			continue;

		if (o->sh[i].sh_type == SHT_NOBITS) {
			if (o->sh[i].sh_flags & SHF_ALLOC)
				o->sec_kind[i] = 3;
			continue;
		}

		if (o->sh[i].sh_offset > size ||
		    o->sh[i].sh_size > size - o->sh[i].sh_offset)
			return -1;

		if ((o->sh[i].sh_flags & SHF_ALLOC) == 0)
			continue;

		if ((o->sh[i].sh_flags & SHF_EXECINSTR) ||
		    starts_with(name, ".text")) {
			o->sec_kind[i] = 1;
		} else {
			/* The fixed script has .data/.bss, but treating other
			 * allocated non-code sections as data lets ordinary
			 * .rodata/.sdata strings in the supplied runtime
			 * objects work too. */
			o->sec_kind[i] = 2;
		}
	}

	if (o->sym_sec < 0 || o->str_sec < 0)
		return -1;
	return 0;
}

static struct Elf64_Sym* link_symbols(struct LinkObj* o, int* n)
{
	struct Elf64_Shdr* ss = &o->sh[o->sym_sec];
	if (ss->sh_size % sizeof(struct Elf64_Sym))
		return 0;
	*n = (int)(ss->sh_size / sizeof(struct Elf64_Sym));
	return (struct Elf64_Sym*)(o->file + ss->sh_offset);
}

static const char* link_strtab(struct LinkObj* o)
{
	return (const char*)o->file + o->sh[o->str_sec].sh_offset;
}

#define STB_WEAK 2

struct GDef {
	char* name;
	unsigned long long addr;
	int bind;
};

struct GDefVec {
	struct GDef* v;
	int n;
	int cap;
};

static void
gdef_add(struct GDefVec* v, const char* name, unsigned long long addr, int bind)
{
	int i;

	if (v->n == v->cap) {
		int nc = v->cap ? v->cap * 2 : 128;
		struct GDef* p = (struct GDef*)realloc(v->v, nc * sizeof(*p));
		if (!p)
			die_plain("lcc: out of memory");
		v->v = p;
		v->cap = nc;
	}

	for (i = 0; i < v->n; ++i) {
		if (u_streq(v->v[i].name, name)) {
			if (bind == STB_GLOBAL && v->v[i].bind == STB_GLOBAL)
				die_plain("lcc: duplicate global symbol");
			if (bind == STB_GLOBAL && v->v[i].bind == STB_WEAK) {
				v->v[i].addr = addr;
				v->v[i].bind = bind;
			}
			return;
		}
	}

	v->v[v->n].name = xstrdup_n(name, u_strlen(name));
	v->v[v->n].addr = addr;
	v->v[v->n].bind = bind;
	++v->n;
}

static int
gdef_find(struct GDefVec* v, const char* name, unsigned long long* addr)
{
	int i;
	for (i = 0; i < v->n; ++i) {
		if (u_streq(v->v[i].name, name)) {
			*addr = v->v[i].addr;
			return 0;
		}
	}
	return -1;
}

static unsigned long long symbol_address(struct LinkObj* o, struct Elf64_Sym* s)
{
	if (s->st_shndx == SHN_UNDEF)
		return 0;
	if (s->st_shndx == SHN_ABS)
		return s->st_value;
	if (s->st_shndx >= o->eh.e_shnum)
		return 0;
	return o->sec_addr[s->st_shndx] + s->st_value;
}

static unsigned long long resolve_symbol(struct LinkObj* o,
					 struct GDefVec* gdefs,
					 struct Elf64_Sym* s,
					 const char* name)
{
	unsigned long long addr;
	if (s->st_shndx != SHN_UNDEF)
		return symbol_address(o, s);
	if (gdef_find(gdefs, name, &addr) < 0) {
		write(STDERR_FD, "lcc: undefined symbol: ", 24);
		write(STDERR_FD, name, u_strlen(name));
		write(STDERR_FD, "\n", 1);
		die_plain("lcc: link aborted");
	}
	return addr;
}

static void patch_i(unsigned char* p, long long imm)
{
	unsigned int x = rd32(p);
	x &= 0x000fffffU;
	x |= ((unsigned)(imm & 0xfff) << 20);
	wr32(p, x);
}

static void patch_s(unsigned char* p, long long imm)
{
	unsigned int x = rd32(p);
	unsigned int u = (unsigned int)imm;
	x &= ~((0x7fU << 25) | (0x1fU << 7));
	x |= ((u >> 5) & 0x7f) << 25;
	x |= (u & 0x1f) << 7;
	wr32(p, x);
}

static void patch_u(unsigned char* p, long long imm)
{
	unsigned int x = rd32(p);
	x &= 0x00000fffU;
	x |= (unsigned int)(imm & 0xfffff000LL);
	wr32(p, x);
}

static void patch_b(unsigned char* p, long long imm)
{
	unsigned int x = rd32(p);
	unsigned int u = (unsigned int)imm;
	x &= 0x01fff07fU;
	x |= ((u >> 12) & 1) << 31;
	x |= ((u >> 5) & 0x3f) << 25;
	x |= ((u >> 1) & 0xf) << 8;
	x |= ((u >> 11) & 1) << 7;
	wr32(p, x);
}

static void patch_j(unsigned char* p, long long imm)
{
	unsigned int x = rd32(p);
	unsigned int u = (unsigned int)imm;
	x &= 0x00000fffU;
	x |= ((u >> 20) & 1) << 31;
	x |= ((u >> 1) & 0x3ff) << 21;
	x |= ((u >> 11) & 1) << 20;
	x |= ((u >> 12) & 0xff) << 12;
	wr32(p, x);
}

/* CB-type (c.beqz/c.bnez), verified against GNU as: inst bits
 * 12,11,10,6,5,4,3,2 hold imm bits 8,4,3,7,6,2,1,5. */
static void patch_rvc_branch(unsigned char* p, long long imm)
{
	unsigned short x = (unsigned short)(p[0] | ((unsigned short)p[1] << 8));
	unsigned u = (unsigned)imm;
	x &= (unsigned short)~0x1c7cU;
	x |= (unsigned short)(((u >> 8) & 1) << 12);
	x |= (unsigned short)(((u >> 4) & 1) << 11);
	x |= (unsigned short)(((u >> 3) & 1) << 10);
	x |= (unsigned short)(((u >> 7) & 1) << 6);
	x |= (unsigned short)(((u >> 6) & 1) << 5);
	x |= (unsigned short)(((u >> 2) & 1) << 4);
	x |= (unsigned short)(((u >> 1) & 1) << 3);
	x |= (unsigned short)(((u >> 5) & 1) << 2);
	wr16(p, x);
}

/* CJ-type (c.j): inst bits 12,11,10,9,8,7,6,5,4:3,2 hold
 * imm bits 11,4,9,8,10,6,7,3,2:1,5. */
static void patch_rvc_jump(unsigned char* p, long long imm)
{
	unsigned short x = (unsigned short)(p[0] | ((unsigned short)p[1] << 8));
	unsigned u = (unsigned)imm;
	x &= (unsigned short)~0x1ffcU;
	x |= (unsigned short)(((u >> 11) & 1) << 12);
	x |= (unsigned short)(((u >> 4) & 1) << 11);
	x |= (unsigned short)(((u >> 9) & 1) << 10);
	x |= (unsigned short)(((u >> 8) & 1) << 9);
	x |= (unsigned short)(((u >> 10) & 1) << 8);
	x |= (unsigned short)(((u >> 6) & 1) << 7);
	x |= (unsigned short)(((u >> 7) & 1) << 6);
	x |= (unsigned short)(((u >> 3) & 1) << 5);
	x |= (unsigned short)(((u >> 2) & 1) << 4);
	x |= (unsigned short)(((u >> 1) & 1) << 3);
	x |= (unsigned short)(((u >> 5) & 1) << 2);
	wr16(p, x);
}

static int find_pcrel_hi(struct LinkObj* o,
			 int target_sec,
			 unsigned long long hi_addr,
			 struct GDefVec* gdefs,
			 unsigned long long* S)
{
	int i, n;
	struct Elf64_Sym* syms = link_symbols(o, &n);
	const char* str = link_strtab(o);
	if (!syms)
		return -1;

	for (i = 0; i < o->eh.e_shnum; ++i) {
		const struct Elf64_Shdr* rs = &o->sh[i];
		const char* rn = section_name(o, i);
		unsigned long k, cnt;
		const struct Elf64_Rela* rela;
		if (rs->sh_type != SHT_RELA ||
		    rs->sh_info != (unsigned)target_sec)
			continue;
		if (!starts_with(rn, ".rela"))
			continue;
		cnt = rs->sh_size / sizeof(struct Elf64_Rela);
		rela = (const struct Elf64_Rela*)(o->file + rs->sh_offset);
		for (k = 0; k < cnt; ++k) {
			if (r_type(rela[k].r_info) == R_RISCV_PCREL_HI20 &&
			    o->sec_addr[target_sec] + rela[k].r_offset ==
				hi_addr) {
				unsigned si = r_sym(rela[k].r_info);
				if (si >= (unsigned)n)
					return -1;
				*S = resolve_symbol(o,
						    gdefs,
						    &syms[si],
						    str + syms[si].st_name);
				return 0;
			}
		}
	}
	return -1;
}

static void apply_relocation(struct LinkObj* o,
			     struct GDefVec* gdefs,
			     unsigned char* final_text,
			     unsigned char* final_data,
			     const struct Elf64_Shdr* rela_sec,
			     const struct Elf64_Rela* r,
			     struct Elf64_Sym* syms,
			     const char* strtab)
{
	unsigned int si = r_sym(r->r_info);
	unsigned int type = r_type(r->r_info);
	int target_sec = (int)rela_sec->sh_info;
	unsigned char* base;
	unsigned char* loc;
	unsigned long long S;
	unsigned long long P;

	if (type == R_RISCV_NONE || type == R_RISCV_RELAX ||
	    type == R_RISCV_ALIGN)
		return;
	if (target_sec < 0 || target_sec >= o->eh.e_shnum)
		die_plain("lcc: bad relocation target section");
	if (!o->sec_kind[target_sec])
		die_plain("lcc: relocation targets a non-loadable section");
	if (si >= 0x7fffffffU)
		die_plain("lcc: bad relocation symbol index");

	S = resolve_symbol(o, gdefs, &syms[si], strtab + syms[si].st_name);
	P = o->sec_addr[target_sec] + r->r_offset;

	if (o->sec_kind[target_sec] == 1) {
		base = final_text;
		loc = base + (unsigned long)(P - TEXT_BASE);
	} else {
		unsigned long long data_base = TEXT_BASE;
		/* The caller has laid out data immediately after text; for a
		 * relocation into data we recover its file-buffer-relative
		 * position from sec_addr. final_data starts at the lowest data
		 * section address, supplied as the section's minimum data
		 * address through g_link_data_base. */
		data_base = g_link_data_base;
		loc = final_data + (unsigned long)(P - data_base);
		base = final_data;
		(void)base;
	}

	switch (type) {
	case R_RISCV_32:
		wr32(loc, (unsigned int)((long long)S + r->r_addend));
		break;
	case R_RISCV_64:
		wr64(loc, (unsigned long long)((long long)S + r->r_addend));
		break;
	case R_RISCV_JAL:
		patch_j(loc, (long long)S + r->r_addend - (long long)P);
		break;
	case R_RISCV_BRANCH:
		patch_b(loc, (long long)S + r->r_addend - (long long)P);
		break;
	case R_RISCV_RVC_BRANCH:
		patch_rvc_branch(loc,
				 (long long)S + r->r_addend - (long long)P);
		break;
	case R_RISCV_RVC_JUMP:
		patch_rvc_jump(loc, (long long)S + r->r_addend - (long long)P);
		break;
	case R_RISCV_CALL:
	case R_RISCV_CALL_PLT: {
		long long d = (long long)S + r->r_addend - (long long)P;
		long long hi = (d + 0x800) >> 12;
		long long lo = d - (hi << 12);
		patch_u(loc, hi << 12);
		patch_i(loc + 4, lo);
		break;
	}
	case R_RISCV_HI20: {
		long long v = (long long)S + r->r_addend;
		long long hi = (v + 0x800) >> 12;
		patch_u(loc, hi << 12);
		break;
	}
	case R_RISCV_LO12_I:
		patch_i(loc, (long long)S + r->r_addend);
		break;
	case R_RISCV_LO12_S:
		patch_s(loc, (long long)S + r->r_addend);
		break;
	case R_RISCV_PCREL_HI20: {
		long long d = (long long)S + r->r_addend - (long long)P;
		long long hi = (d + 0x800) >> 12;
		patch_u(loc, hi << 12);
		break;
	}
	case R_RISCV_PCREL_LO12_I: {
		unsigned long long hi_addr = symbol_address(o, &syms[si]);
		unsigned long long target;
		long long d;
		if (find_pcrel_hi(o, target_sec, hi_addr, gdefs, &target) < 0)
			die_plain("lcc: unmatched PCREL_LO12 relocation");
		d = (long long)target + r->r_addend - (long long)hi_addr;
		patch_i(loc, d);
		break;
	}
	case R_RISCV_PCREL_LO12_S: {
		unsigned long long hi_addr = symbol_address(o, &syms[si]);
		unsigned long long target;
		if (find_pcrel_hi(o, target_sec, hi_addr, gdefs, &target) < 0)
			die_plain("lcc: unmatched PCREL_LO12 relocation");
		patch_s(loc,
			(long long)target + r->r_addend - (long long)hi_addr);
		break;
	}
	default:
		die_plain("lcc: unsupported RISC-V relocation");
	}
}

static int link_final(struct ByteVec* user_elf, const char* outpath)
{
	const char* names[5];
	char* owned_paths[5];
	unsigned char* files[5] = {0};
	unsigned long sizes[5] = {0};
	struct LinkObj objs[5];
	int i, j;
	unsigned long long text_cur = TEXT_BASE;
	unsigned long long text_end;
	unsigned long long data_cur;
	unsigned long long bss_cur;
	unsigned long long data_end;
	int have_data = 0;
	int have_bss = 0;
	struct ByteVec final_text, final_data, final_elf;
	struct GDefVec gdefs = {0};
	unsigned long long start_addr = 0;

	owned_paths[0] = lib_path("crt0.o");
	owned_paths[1] = 0;
	owned_paths[2] = lib_path("ulib.o");
	owned_paths[3] = lib_path("malloc.o");
	owned_paths[4] = lib_path("stdio.o");
	names[0] = owned_paths[0];
	names[1] = "<user.o>";
	names[2] = owned_paths[2];
	names[3] = owned_paths[3];
	names[4] = owned_paths[4];
	files[1] = user_elf->p;
	sizes[1] = user_elf->n;

	for (i = 0; i < 5; ++i) {
		if (i == 1)
			continue;
		if (!owned_paths[i])
			return -1;
		if (file_read_all(names[i], &files[i], &sizes[i]) < 0) {
			write(
			    STDERR_FD, "lcc: cannot read runtime object: ", 33);
			write(STDERR_FD, names[i], u_strlen(names[i]));
			write(STDERR_FD, "\n", 1);
			return -1;
		}
	}

	for (i = 0; i < 5; ++i) {
		if (linkobj_parse(&objs[i], files[i], sizes[i]) < 0)
			die_plain("lcc: invalid RISC-V ET_REL object");
	}

	/* First pass: concatenate all executable sections. */
	for (i = 0; i < 5; ++i) {
		for (j = 0; j < objs[i].eh.e_shnum; ++j) {
			if (objs[i].sec_kind[j] != 1)
				continue;
			text_cur = align_up((unsigned long)text_cur,
					    objs[i].sh[j].sh_addralign
						? objs[i].sh[j].sh_addralign
						: 4);
			objs[i].sec_addr[j] = text_cur;
			text_cur += objs[i].sh[j].sh_size;
		}
	}
	text_end = text_cur;

	/* Second pass: all other allocated PROGBITS sections, corresponding to
	 * .data/.rodata/etc. in this tiny linker's orphan policy. */
	data_cur = align_up((unsigned long)text_end, PAGE_SIZE);
	g_link_data_base = data_cur;
	for (i = 0; i < 5; ++i) {
		for (j = 0; j < objs[i].eh.e_shnum; ++j) {
			if (objs[i].sec_kind[j] != 2)
				continue;
			have_data = 1;
			data_cur = align_up((unsigned long)data_cur,
					    objs[i].sh[j].sh_addralign
						? objs[i].sh[j].sh_addralign
						: 1);
			objs[i].sec_addr[j] = data_cur;
			data_cur += objs[i].sh[j].sh_size;
		}
	}
	data_end = data_cur;

	/* Third pass: zero-initialized memory. */
	bss_cur = data_cur;
	for (i = 0; i < 5; ++i) {
		for (j = 0; j < objs[i].eh.e_shnum; ++j) {
			if (objs[i].sec_kind[j] != 3)
				continue;
			have_bss = 1;
			bss_cur = align_up((unsigned long)bss_cur,
					   objs[i].sh[j].sh_addralign
					       ? objs[i].sh[j].sh_addralign
					       : 1);
			objs[i].sec_addr[j] = bss_cur;
			bss_cur += objs[i].sh[j].sh_size;
		}
	}

	bv_init(&final_text);
	bv_init(&final_data);
	if (bv_resize(&final_text, (unsigned long)(text_end - TEXT_BASE)) < 0)
		die_plain("lcc: out of memory");
	if (bv_resize(&final_data,
		      have_data ? (unsigned long)(data_cur - g_link_data_base)
				: 0) < 0)
		die_plain("lcc: out of memory");

	for (i = 0; i < 5; ++i) {
		for (j = 0; j < objs[i].eh.e_shnum; ++j) {
			if (objs[i].sec_kind[j] == 1 &&
			    objs[i].sh[j].sh_type != SHT_NOBITS) {
				mc_memcpy(final_text.p +
					      (objs[i].sec_addr[j] - TEXT_BASE),
					  files[i] + objs[i].sh[j].sh_offset,
					  objs[i].sh[j].sh_size);
			} else if (objs[i].sec_kind[j] == 2 &&
				   objs[i].sh[j].sh_type != SHT_NOBITS) {
				mc_memcpy(final_data.p + (objs[i].sec_addr[j] -
							  g_link_data_base),
					  files[i] + objs[i].sh[j].sh_offset,
					  objs[i].sh[j].sh_size);
			}
		}
	}

	/* Collect global definitions. crt0.o is deliberately first, so _start
	 * must be its first text symbol at exactly 0x1000. */
	for (i = 0; i < 5; ++i) {
		int ns;
		struct Elf64_Sym* syms = link_symbols(&objs[i], &ns);
		const char* str = link_strtab(&objs[i]);
		if (!syms)
			die_plain("lcc: malformed symbol table");
		for (j = 1; j < ns; ++j) {
			int bind = syms[j].st_info >> 4;
			if (bind != STB_GLOBAL && bind != STB_WEAK)
				continue;
			if (syms[j].st_shndx == SHN_UNDEF)
				continue;
			{
				const char* name = str + syms[j].st_name;
				unsigned long long addr =
				    symbol_address(&objs[i], &syms[j]);
				gdef_add(&gdefs, name, addr, bind);
				if (u_streq(name, "_start"))
					start_addr = addr;
			}
		}
	}

	if (start_addr != TEXT_BASE)
		die_plain("lcc: _start must be provided by crt0.o and resolve "
			  "to 0x1000");

	/* Resolve every relocation section against the already laid-out memory.
	 */
	for (i = 0; i < 5; ++i) {
		int ns;
		struct Elf64_Sym* syms = link_symbols(&objs[i], &ns);
		const char* str = link_strtab(&objs[i]);
		if (!syms)
			die_plain("lcc: malformed symbol table");
		for (j = 0; j < objs[i].eh.e_shnum; ++j) {
			unsigned long k, cnt;
			const struct Elf64_Rela* rs;
			if (objs[i].sh[j].sh_type != SHT_RELA)
				continue;
			if (objs[i].sh[j].sh_info >=
			    (unsigned)objs[i].eh.e_shnum)
				die_plain("lcc: malformed relocation section");
			if (!objs[i].sec_kind[objs[i].sh[j].sh_info])
				continue;
			cnt = objs[i].sh[j].sh_size / sizeof(struct Elf64_Rela);
			rs =
			    (const struct Elf64_Rela*)(files[i] +
						       objs[i].sh[j].sh_offset);
			for (k = 0; k < cnt; ++k)
				apply_relocation(&objs[i],
						 &gdefs,
						 final_text.p,
						 final_data.p,
						 &objs[i].sh[j],
						 &rs[k],
						 syms,
						 str);
		}
	}

	{
		struct Elf64_Ehdr eh;
		struct Elf64_Phdr ph;
		unsigned long text_off = PAGE_SIZE;
		unsigned long file_end;
		unsigned long mem_end;

		if (have_data)
			file_end = (unsigned long)data_end;
		else
			file_end = (unsigned long)text_end;

		if (have_bss)
			mem_end = (unsigned long)bss_cur;
		else if (have_data)
			mem_end = (unsigned long)data_end;
		else
			mem_end = (unsigned long)text_end;

		mc_memset(&eh, 0, sizeof(eh));
		mc_memset(&ph, 0, sizeof(ph));

		eh.e_ident[0] = 0x7f;
		eh.e_ident[1] = 'E';
		eh.e_ident[2] = 'L';
		eh.e_ident[3] = 'F';
		eh.e_ident[4] = ELFCLASS64;
		eh.e_ident[5] = ELFDATA2LSB;
		eh.e_ident[6] = 1;
		eh.e_type = ET_EXEC;
		eh.e_machine = EM_RISCV;
		eh.e_version = 1;
		eh.e_entry = start_addr;
		eh.e_phoff = sizeof(struct Elf64_Ehdr);
		eh.e_phentsize = sizeof(struct Elf64_Phdr);
		eh.e_phnum = 1;
		eh.e_ehsize = sizeof(struct Elf64_Ehdr);

		ph.p_type = PT_LOAD;
		ph.p_flags = PF_R | PF_X;
		ph.p_offset = TEXT_BASE;
		ph.p_vaddr = TEXT_BASE;
		ph.p_paddr = TEXT_BASE;
		ph.p_filesz = file_end - TEXT_BASE;
		ph.p_memsz = mem_end - TEXT_BASE;
		ph.p_align = PAGE_SIZE;

		bv_init(&final_elf);
		if (bv_resize(&final_elf, text_off + final_text.n) < 0)
			die_plain("lcc: out of memory");
		mc_memcpy(final_elf.p, &eh, sizeof(eh));
		mc_memcpy(final_elf.p + sizeof(eh), &ph, sizeof(ph));
		mc_memcpy(final_elf.p + text_off, final_text.p, final_text.n);
		if (have_data) {
			if (bv_resize(&final_elf, (unsigned long)data_end) < 0)
				die_plain("lcc: out of memory");
			mc_memcpy(final_elf.p + g_link_data_base,
				  final_data.p,
				  final_data.n);
		}

		if (file_write_all(outpath, final_elf.p, final_elf.n) < 0)
			die_plain("lcc: cannot write output file");
	}

	for (i = 0; i < 5; ++i) {
		if (i != 1)
			free(files[i]);
		free(objs[i].names);
		free(objs[i].sec_addr);
		free(objs[i].sec_kind);
		free(owned_paths[i]);
	}
	free(gdefs.v);
	free(final_text.p);
	free(final_data.p);
	return 0;
}

static void dump_expr(struct Expr* e, int d)
{
	int i;
	while (d--)
		write(STDOUT_FD, "  ", 2);
	switch (e->kind) {
	case EX_INT:
		write(STDOUT_FD, "Int\n", 4);
		break;
	case EX_CHAR:
		write(STDOUT_FD, "Char\n", 5);
		break;
	case EX_STRING:
		write(STDOUT_FD, "String\n", 7);
		break;
	case EX_VAR:
		write(STDOUT_FD, "Var ", 4);
		write(STDOUT_FD, e->sym->name, u_strlen(e->sym->name));
		write(STDOUT_FD, "\n", 1);
		break;
	case EX_UNARY:
		write(STDOUT_FD, "Unary ", 6);
		write(STDOUT_FD, e->op, u_strlen(e->op));
		write(STDOUT_FD, "\n", 1);
		dump_expr(e->a, d + 1);
		break;
	case EX_BINARY:
		write(STDOUT_FD, "Binary ", 7);
		write(STDOUT_FD, e->op, u_strlen(e->op));
		write(STDOUT_FD, "\n", 1);
		dump_expr(e->a, d + 1);
		dump_expr(e->b, d + 1);
		break;
	case EX_ASSIGN:
		write(STDOUT_FD, "Assign ", 7);
		write(STDOUT_FD, e->op, u_strlen(e->op));
		write(STDOUT_FD, "\n", 1);
		dump_expr(e->a, d + 1);
		dump_expr(e->b, d + 1);
		break;
	case EX_CALL:
		write(STDOUT_FD, "Call ", 5);
		write(STDOUT_FD, e->sym->name, u_strlen(e->sym->name));
		write(STDOUT_FD, "\n", 1);
		for (i = 0; i < e->args.n; ++i)
			dump_expr(e->args.v[i], d + 1);
		break;
	case EX_INDEX:
		write(STDOUT_FD, "Index\n", 6);
		dump_expr(e->a, d + 1);
		dump_expr(e->b, d + 1);
		break;
	case EX_COND:
		write(STDOUT_FD, "Cond\n", 5);
		dump_expr(e->a, d + 1);
		dump_expr(e->b, d + 1);
		dump_expr(e->c, d + 1);
		break;
	case EX_SIZEOF:
		write(STDOUT_FD, "Sizeof\n", 7);
		break;
	}
}

static void dump_stmt(struct Stmt* s, int d)
{
	int i;
	while (d--)
		write(STDOUT_FD, "  ", 2);
	switch (s->kind) {
	case ST_BLOCK:
		write(STDOUT_FD, "Block\n", 6);
		for (i = 0; i < s->body.n; ++i)
			dump_stmt(s->body.v[i], d + 1);
		break;
	case ST_DECL:
		write(STDOUT_FD, "Decl ", 5);
		write(STDOUT_FD, s->sym->name, u_strlen(s->sym->name));
		write(STDOUT_FD, "\n", 1);
		if (s->e)
			dump_expr(s->e, d + 1);
		break;
	case ST_EXPR:
		write(STDOUT_FD, "Expr\n", 5);
		dump_expr(s->e, d + 1);
		break;
	case ST_IF:
		write(STDOUT_FD, "If\n", 3);
		dump_expr(s->e, d + 1);
		dump_stmt(s->s1, d + 1);
		if (s->s2)
			dump_stmt(s->s2, d + 1);
		break;
	case ST_WHILE:
		write(STDOUT_FD, "While\n", 6);
		dump_expr(s->e, d + 1);
		dump_stmt(s->s1, d + 1);
		break;
	case ST_FOR:
		write(STDOUT_FD, "For\n", 4);
		if (s->s1)
			dump_stmt(s->s1, d + 1);
		if (s->e)
			dump_expr(s->e, d + 1);
		if (s->e2)
			dump_expr(s->e2, d + 1);
		dump_stmt(s->s2, d + 1);
		break;
	case ST_RETURN:
		write(STDOUT_FD, "Return\n", 7);
		if (s->e)
			dump_expr(s->e, d + 1);
		break;
	case ST_BREAK:
		write(STDOUT_FD, "Break\n", 6);
		break;
	case ST_CONTINUE:
		write(STDOUT_FD, "Continue\n", 9);
		break;
	}
}

struct Options {
	const char* input;
	const char* output;
	const char* include_dir;
	const char* lib_dir;
	int only_c;
	int only_s;
	int only_e;
	int dump_ast;
};

static int asm_emit_bytes(struct ByteVec* v,
			  const char* section,
			  const unsigned char* p,
			  unsigned long n)
{
	unsigned long i;
	char h[3];
	if (bv_append(v, section, u_strlen(section)) < 0)
		return -1;
	if (bv_append(v, ".byte ", 6) < 0)
		return -1;
	for (i = 0; i < n; ++i) {
		put_hex8(h, p[i]);
		if (i && bv_append(v, ", ", 2) < 0)
			return -1;
		if (bv_append(v, "0x", 2) < 0)
			return -1;
		if (bv_append(v, h, 2) < 0)
			return -1;
	}
	if (bv_append(v, "\n", 1) < 0)
		return -1;
	return 0;
}

static int write_assembly_listing(struct Object* o, const char* path)
{
	struct ByteVec out;
	char buf[64];
	bv_init(&out);
	if (bv_append(&out, ".section .text\n", 15) < 0)
		return -1;
	if (bv_append(&out, ".align 2\n", 9) < 0)
		return -1;
	if (o->text.n && asm_emit_bytes(&out, "", o->text.p, o->text.n) < 0)
		return -1;
	if (bv_append(&out, ".section .data\n", 15) < 0)
		return -1;
	if (bv_append(&out, ".align 3\n", 9) < 0)
		return -1;
	if (o->data.n && asm_emit_bytes(&out, "", o->data.p, o->data.n) < 0)
		return -1;
	if (bv_append(&out, ".section .bss\n", 14) < 0)
		return -1;
	if (o->bss_size) {
		put_u64_dec(buf, o->bss_size);
		if (bv_append(&out, ".zero ", 6) < 0 ||
		    bv_append(&out, buf, u_strlen(buf)) < 0 ||
		    bv_append(&out, "\n", 1) < 0)
			return -1;
	}
	if (file_write_all(path, out.p, out.n) < 0)
		return -1;
	free(out.p);
	return 0;
}

static void usage(void)
{
	const char* s = "usage: lcc <input.mc> -o <output> [options]\n\n"
			"  -o <file>       output file\n"
			"  -I <dir>        include directory (default /lib)\n"
			"  -L <dir>        library directory (default /lib)\n"
			"  -c              emit ELF64 ET_REL object\n"
			"  -S              dump generated machine code bytes\n"
			"  -E              emit include-expanded source\n"
			"  --dump-ast      dump AST\n"
			"  -h, --help      show help\n";
	write(STDOUT_FD, s, u_strlen(s));
}

static int parse_options(int argc, char** argv, struct Options* o)
{
	int i;
	mc_memset(o, 0, sizeof(*o));
	o->include_dir = INCLUDE_DIR;
	o->lib_dir = LIB_DIR;
	for (i = 1; i < argc; ++i) {
		if (u_streq(argv[i], "-h") || u_streq(argv[i], "--help")) {
			usage();
			return 1;
		}
		if (u_streq(argv[i], "-o")) {
			if (i + 1 >= argc)
				die_plain("lcc: -o requires an argument");
			o->output = argv[++i];
			continue;
		}
		if (u_streq(argv[i], "-I")) {
			if (i + 1 >= argc)
				die_plain("lcc: -I requires an argument");
			o->include_dir = argv[++i];
			continue;
		}
		if (u_streq(argv[i], "-L")) {
			if (i + 1 >= argc)
				die_plain("lcc: -L requires an argument");
			o->lib_dir = argv[++i];
			continue;
		}
		if (u_streq(argv[i], "-c")) {
			o->only_c = 1;
			continue;
		}
		if (u_streq(argv[i], "-S")) {
			o->only_s = 1;
			continue;
		}
		if (u_streq(argv[i], "-E") || u_streq(argv[i], "-e")) {
			o->only_e = 1;
			continue;
		}
		if (u_streq(argv[i], "--dump-ast")) {
			o->dump_ast = 1;
			continue;
		}
		if (argv[i][0] == '-')
			die_plain("lcc: unknown option");
		if (o->input)
			die_plain("lcc: multiple input files");
		o->input = argv[i];
	}
	if (!o->input)
		return -1;
	if (!o->only_e && !o->output)
		die_plain("lcc: -o is required");
	return 0;
}

int main(int argc, char** argv)
{
	struct Options opt;
	int pr = parse_options(argc, argv, &opt);
	if (pr == 1)
		return 0;
	if (pr < 0) {
		usage();
		return 1;
	}
	g_include_dir = opt.include_dir;
	g_lib_dir = opt.lib_dir;
	{
		char* source = expand_file(opt.input, 0);
		struct TokenVec tv;
		struct Parser parser;
		struct CodeGen cg;
		struct ByteVec objelf;
		if (!source)
			die_plain("lcc: cannot read input or included file");
		if (opt.only_e) {
			if (opt.output && u_streq(opt.output, "-")) {
				write(STDOUT_FD, source, u_strlen(source));
			} else if (opt.output) {
				if (file_write_all(opt.output,
						   (unsigned char*)source,
						   u_strlen(source)) < 0)
					die_plain("lcc: cannot write output");
			} else
				write(STDOUT_FD, source, u_strlen(source));
			free(source);
			return 0;
		}
		if (lex_all(opt.input, source, &tv) < 0)
			return 1;
		if (opt.dump_ast) {
			parser_init(&parser, opt.input, &tv);
			parse_translation_unit(&parser);
			{
				int i;
				for (i = 0; i < parser.funcs.n; ++i) {
					write(STDOUT_FD, "Function ", 9);
					write(STDOUT_FD,
					      parser.funcs.v[i].sym->name,
					      u_strlen(
						  parser.funcs.v[i].sym->name));
					write(STDOUT_FD, "\n", 1);
					dump_stmt(parser.funcs.v[i].body, 1);
				}
			}
			free(source);
			return 0;
		}
		parser_init(&parser, opt.input, &tv);
		parse_translation_unit(&parser);
		mc_memset(&cg, 0, sizeof(cg));
		cg.parser = &parser;
		generate_object(&cg);
		bv_init(&objelf);
		if (object_to_elf(&cg.obj, &objelf) < 0)
			die_plain("lcc: cannot build object");
		if (opt.only_c) {
			if (file_write_all(opt.output, objelf.p, objelf.n) < 0)
				die_plain("lcc: cannot write object");
			return 0;
		}
		if (opt.only_s) {
			if (write_assembly_listing(&cg.obj, opt.output) < 0)
				die_plain("lcc: cannot write assembly listing");
			return 0;
		}
		if (link_final(&objelf, opt.output) < 0)
			die_plain("lcc: link failed");
		return 0;
	}
}
