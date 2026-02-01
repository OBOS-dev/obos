/*
 * oboskrnl/cmdline.c
 *
 * Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <klog.h>
#include <memmanip.h>
#include <cmdline.h>

#include <allocators/base.h>

#include <mm/bare_map.h>

OBOS_PAGEABLE_VARIABLE const char* OBOS_KernelCmdLine;
OBOS_PAGEABLE_VARIABLE const char* volatile OBOS_InitrdBinary;
OBOS_PAGEABLE_VARIABLE size_t volatile OBOS_InitrdSize;
OBOS_PAGEABLE_VARIABLE char** OBOS_argv;
OBOS_PAGEABLE_VARIABLE size_t OBOS_argc;
OBOS_PAGEABLE_VARIABLE size_t OBOS_InitArgumentsStart = SIZE_MAX;
OBOS_PAGEABLE_VARIABLE size_t OBOS_InitArgumentsCount;

static const char* const help_message =
"OBOSKRNL usage:\n"
"NOTE: Any amount of dashes ('-') can be used at the beginning of the option or flag.\n"
"--initrd-module=name: The name or path of the initrd module.\n"
"--initrd-driver-module=name: The name or path of the initrd driver module.\n"
"--load-modules=name[,name]: If an initrd driver is specified, then 'name' is an absolute path\n"
"                            in the initrd, otherwise it is the name of a module to load as a driver.\n"
"--mount-initrd=pathspec: Mounts the InitRD at pathspec if specified, otherwise the initrd is left unmounted\n"
"                         when 'init' is called.\n"
"--root-fs-uuid=uuid: Specifies the partition to mount as root. If set to 'initrd', the initrd\n"
"                     is used as root.\n"
"--root-fs-partid=partid: Specifies the partition to mount as root. If set to 'initrd', the initrd\n"
"                     is used as root.\n"
"--working-set-cap=bytes: Specifies the kernel's working-set size in bytes.\n"
"--initial-swap-size=bytes: Specifies the size (in bytes) of the initial, in-ram swap.\n"
"--log-level=integer: Specifies the log level of the kernel, 0 meaning all, 4 meaning none.\n"
"--disable-network-error-logs: Disable error logs from the network stack\n"
"--init-path=path: Specifies the path of init. If not present, assumes /init.\n"
"--init-args: Special argument, makes the kernel assume all following arguments are to be passed to the init process.\n"
"--no-init: Disables loading the init process.\n"
"--acpi-no-osi: Don't create the _OSI method when building the namespace. For more info, see documenation for UACPI_FLAG_NO_OSI.\n"
"--acpi-bad-xsdt: Use the RSDT, even if the XSDT is present. For more info, see documenation for UACPI_FLAG_BAD_XSDT.\n"
"--no-smp: Disables SMP. Has the equivalent effect of passing OBOS_UP at build-time.\n"
"--pnp-module-path=pathspec: Where to find kernel modules for PnP during kernel init.\n"
"--disable-libc-log: Disables the logs from the C library (see Sys_LibcLog) .\n"
"--disable-syscall-error-log: Makes all syscall logs happen at DEBUG level.\n"
"--disable-syscall-logs: Disables all syscall logs.\n"
"--tjec-random-access: Makes the underlying TJEC memory accessing randomized.\n"
"--tjec-max-memory-size=bytes: Specifies the maximum amount of memory TJEC is allowed to allocate.\n"
"--tjec-no-fips: Tells TJEC to not over sample per block of bits generated.\n"
"--tjec-no-lag-predictor: Disables TJEC LAG Predictor health checks.\n"
"--tjec-max-acc-loop-bits=<1-8>: Specifies a maximum number of random additional memory accesses TJEC makes per block in 2^k, default k=7 or 128.\n"
"--tjec-max-hash-loop-bits=<1-8>: Specifies a maximum number of random additional hash iterations TJEC makes per block in 2^k, default k=3 or 8.\n"
"--tjec-osr=<1-255>: Specifies the over sampling ratio for TJEC, in other words, how many blocks to collect per block generated.\n"
"--x86-disable-tsc: (x86 only) Disables use of the TSC.\n"
"--help: Displays this help message.\n";

struct cmd_allocation_header
{
    size_t alloc_size;
    // set to true if the bump allocator (OBOS_BasicMMAllocatePages) was used.
    bool basicmm;
};

static void* cmd_malloc(size_t sz)
{
    struct cmd_allocation_header* blk = nullptr;
    if (OBOS_KernelAllocator)
    {
        blk = Allocate(OBOS_KernelAllocator, sz+sizeof(*blk), nullptr);
        blk->basicmm = false;
        blk->alloc_size = sz;
        return blk + 1;
    }
    sz += sizeof(*blk);
    if (sz % OBOS_PAGE_SIZE)
        sz += (OBOS_PAGE_SIZE-(sz%OBOS_PAGE_SIZE));
    blk = OBOS_BasicMMAllocatePages(sz, nullptr);
    blk->basicmm = true;
    blk->alloc_size = sz;
    return blk + 1;
}

static void* cmd_calloc(size_t nobj, size_t szobj)
{
    size_t sz = nobj * szobj;
    return memzero(cmd_malloc(sz), sz);
}

static void cmd_free(void* buf)
{
    struct cmd_allocation_header* hdr = buf;
    hdr--;
    if (hdr->basicmm)
    {
        memset(buf, 0x11, hdr->alloc_size);
        return;
    }
    Free(OBOS_KernelAllocator, hdr, hdr->alloc_size+sizeof(*hdr));
}

static void* cmd_realloc(void* buf, size_t newsize)
{
    if (!buf)
        return cmd_malloc(newsize);
    if (!newsize)
    {
        cmd_free(buf);
        return nullptr;
    }
    struct cmd_allocation_header* hdr = buf;
    hdr--;
    if (hdr->basicmm)
    {
        newsize += sizeof(*hdr);
        if (newsize % OBOS_PAGE_SIZE)
            newsize += (OBOS_PAGE_SIZE-(newsize%OBOS_PAGE_SIZE));
        if (newsize == hdr->alloc_size)
            return buf;
        hdr->alloc_size = newsize;
        void* newbuf = cmd_malloc(newsize);
        if (newsize < hdr->alloc_size)
            memcpy(newbuf, hdr, newsize);
        else
            memcpy(newbuf, hdr, hdr->alloc_size+sizeof(*hdr));
        cmd_free(buf); // probably a no-op, but do it anyway.
        return newbuf;
    }
    size_t oldsz = hdr->alloc_size;
    hdr->alloc_size = newsize;
    hdr = Reallocate(OBOS_KernelAllocator, hdr, sizeof(*hdr)+newsize, sizeof(*hdr)+oldsz, nullptr);
    return hdr + 1;
}

// Parses the command line into argv and argc
void OBOS_ParseCMDLine()
{
    size_t cmdlinelen = OBOS_KernelCmdLine ? strlen(OBOS_KernelCmdLine) : 0;
    if (!cmdlinelen)
        return;
    
    for (const char* iter = OBOS_KernelCmdLine; iter < (OBOS_KernelCmdLine + cmdlinelen); )
    {
        OBOS_argv = cmd_realloc(OBOS_argv, (++OBOS_argc)*sizeof(const char* const));
        size_t arg_len = 0;
        if (*iter == '\"' || *iter == '\'')
        {
            char delim = *iter;
            size_t off = 0;
            off = strchr(iter+1, delim);
            while (iter[off - 1] == '\\' && off < cmdlinelen)
                off = strchr(iter + off, delim);
            if (off == cmdlinelen - (iter-OBOS_KernelCmdLine))
                arg_len = off;
            else
                arg_len = off - 1;
            iter++;
        }
        else 
        {
            arg_len = strchr(iter, ' ');
            if (arg_len != cmdlinelen-(iter-OBOS_KernelCmdLine))
                arg_len--;
        }
        OBOS_argv[OBOS_argc - 1] = memcpy(cmd_calloc(arg_len + 1, sizeof(char)), iter, arg_len);
        iter += arg_len+1;
    }
    for (size_t i = 0; i < OBOS_argc; i++)
    {
        // *-(opt)
        const char* arg = OBOS_argv[i];
        while (*arg == '-')
            arg++;
        size_t arglen = strlen(arg);
        if (!arglen)
            continue;
        size_t optlen = strchr(arg, '=');
        if (arglen != optlen || arg[arglen - 1] == '=')
            continue;
        if (strncmp("init-args", arg, optlen))
        {
            if ((i + 1) == OBOS_argc)
                break;
            OBOS_InitArgumentsStart = i+1;
            OBOS_InitArgumentsCount = OBOS_argc - OBOS_InitArgumentsStart;
            OBOS_argc = i;
            break;
        }
    }
    if (OBOS_GetOPTF("help"))
        printf("%s", help_message);
}

#define user_alloc(sz) \
(OBOS_KernelAllocator ? \
    ZeroAllocate(OBOS_KernelAllocator, sz, 1, nullptr) : \
    OBOS_BasicMMAllocatePages(sz, nullptr)\
)

char* OBOS_GetOPTS(const char* opt)
{
    for (size_t i = 0; i < OBOS_argc; i++)
    {
        // *-(opt)=[value]
        const char* arg = OBOS_argv[i];
        while (*arg == '-')
            arg++;
        size_t arglen = strlen(arg);
        if (!arglen)
            continue;
        size_t optlen = strchr(arg, '=');
        if (arglen != optlen || arg[arglen - 1] == '=')
            optlen--;
        // Wut?
        // if (arglen == optlen && arg[arglen - 1] != '=')
        //     continue;
        if (strncmp(arg, opt, optlen))
        {
            if (i == (OBOS_argc - 1) && optlen == arglen)
                return nullptr;
            if (optlen == arglen)
            {
                size_t valuelen = strlen(OBOS_argv[i+1]);
                return memcpy(user_alloc(valuelen+1), OBOS_argv[i+1], valuelen);
            }
            size_t valuelen = -1+arglen-optlen;
            arg += optlen+1;
            return memcpy(user_alloc(valuelen+1), arg, valuelen);
        }
    }
    return nullptr;
}
static uint64_t dec2bin(const char* str, size_t size)
{
	uint64_t ret = 0;
	for (size_t i = 0; i < size; i++)
	{
		if ((str[i] - '0') < 0 || (str[i] - '0') > 9)
			continue;
		ret *= 10;
		ret += str[i] - '0';
	}
	return ret;
}
static uint64_t hex2bin(const char* str, size_t size)
{
	uint64_t ret = 0;
	str += *str == '\n';
	for (int i = size - 1, j = 0; i > -1; i--, j++)
	{
		char c = str[i];
		uintptr_t digit = 0;
		switch (c)
		{
		case '0':
		case '1':
		case '2':
		case '3':
		case '4':
		case '5':
		case '6':
		case '7':
		case '8':
		case '9':
			digit = c - '0';
			break;
		case 'A':
		case 'B':
		case 'C':
		case 'D':
		case 'E':
		case 'F':
			digit = (c - 'A') + 10;
			break;
		case 'a':
		case 'b':
		case 'c':
		case 'd':
		case 'e':
		case 'f':
			digit = (c - 'a') + 10;
			break;
		default:
			break;
		}
		ret |= digit << (j * 4);
	}
	return ret;
}
static uint64_t oct2bin(const char* str, size_t size)
{
	uint64_t n = 0;
	const char* c = str;
	while (size-- > 0) 
		n = n * 8 + (uint64_t)(*c++ - '0');
	return n;
}
static bool isNumber(char ch)
{
	char temp = ch - '0';
	return temp >= 0 && temp < 10;
}
static uint64_t strtoull(const char* str, const char** endptr, int base)
{
	while (!isNumber(*str) && *str)
        str++;
    if (!(*str))
    {
        if (endptr)
            *endptr = nullptr;
        return 0;
    }
	if (!base)
	{
		base = 10;
		if (*(str+1) == 'x' || *(str+1) == 'X')
		{
            base = 16;
            str += 2;
        }
		else if (*str == '0')
		{
			base = 8;
			str++;
		}
	}
	size_t sz = 0;
	while (isNumber(*(str + sz)))
		sz++;
	if (endptr)
		*endptr = (str + sz);
	switch (base)
	{
	case 10:
		return dec2bin(str, sz);
	case 16:
		return hex2bin(str, sz);
	case 8:
		return oct2bin(str, sz);
	default:
		break;
	}
	return 0xffffffffffffffff;
}
__attribute__((alias("strtoull"))) uint64_t OBOSH_StrToULL(const char* str, const char** endptr, int base);

uint64_t OBOS_GetOPTD(const char* opt)
{
    return OBOS_GetOPTD_Ex(opt, UINT64_MAX);
}
uint64_t OBOS_GetOPTD_Ex(const char* opt, uint64_t default_value)
{
    char* val = OBOS_GetOPTS(opt);
    if (!val)
        return default_value;
    uint64_t dec = strtoull(val, nullptr, 0);
    Free(OBOS_KernelAllocator, val, strlen(val)+1);
    return dec;
}

bool OBOS_GetOPTF(const char* opt)
{
    for (size_t i = 0; i < OBOS_argc; i++)
    {
        // *-(opt)
        const char* arg = OBOS_argv[i];
        while (*arg == '-')
            arg++;
        size_t arglen = strlen(arg);
        if (!arglen)
            continue;
        size_t optlen = strchr(arg, '=');
        if (arglen != optlen || arg[arglen - 1] == '=')
            continue;
        if (strncmp(opt, arg, optlen))
            return true;
    }
    return false;
}
