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

#include <uacpi_libc.h>

const char* OBOS_KernelCmdLine;
const char* OBOS_InitrdBinary;
size_t OBOS_InitrdSize;
char** OBOS_argv;
size_t OBOS_argc;

// Parses the command line into argv and argc
void OBOS_ParseCMDLine()
{
    size_t cmdlinelen = OBOS_KernelCmdLine ? strlen(OBOS_KernelCmdLine) : 0;
    if (!cmdlinelen)
        return;
    
    for (const char* iter = OBOS_KernelCmdLine; iter < (OBOS_KernelCmdLine + cmdlinelen); )
    {
        OBOS_argv = OBOS_KernelAllocator->Reallocate(OBOS_KernelAllocator, OBOS_argv, (++OBOS_argc)*sizeof(const char* const), nullptr);
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
        OBOS_argv[OBOS_argc - 1] = memcpy(OBOS_KernelAllocator->ZeroAllocate(OBOS_KernelAllocator, arg_len + 1, sizeof(char), nullptr), iter, arg_len);
        iter += arg_len+1;
    }
    if (OBOS_GetOPTF("help"))
    {
        static const char* const help_message = 
            "OBOSKRNL usage:\n"
            "NOTE: Any amount of dashes ('-') can be used at the beginning of the option or flag.\n"
            "--enable-kdbg: Enables the kernel debugger at boot. Not all architectures support this.\n"
            "--initrd-module=name: The name or path of the initrd module.\n"
            "--initrd-driver-module=name: The name or path of the initrd driver module.\n"
            "--load-modules=name[,name]: If an initrd driver is avaliable, then 'name' is a path relative to the initrd, otherwise\n"
            "                            it is the name of a module to load as a driver.\n"
            "--mount-initrd=pathspec: Mounts the InitRD at pathspec if specified, otherwise the initrd is left unmounted."
            "--root-fs-uuid=uuid: Specifies the partition to mount as root. If set to 'initrd', the initrd"
            "                     is used as root."
            "--root-fs-partid=partid: Specifies the partition to mount as root. If set to 'initrd', the initrd"
            "                         is used as root."
            "--help: Displays this help message.\n";
        printf("%s", help_message);
    }
}
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
        if (arglen == optlen && arg[arglen - 1] != '=')
            continue;
        if (uacpi_strncmp(arg, opt, optlen) == 0)
        {
            if (i == (OBOS_argc - 1) && optlen == arglen)
                return nullptr;
            if (optlen == arglen)
            {
                size_t valuelen = strlen(OBOS_argv[i+1]);
                return memcpy(OBOS_KernelAllocator->ZeroAllocate(OBOS_KernelAllocator, valuelen+1, sizeof(char), nullptr), OBOS_argv[i+1], valuelen);
            }
            size_t valuelen = arglen-optlen;
            arg += optlen+1;
            return memcpy(OBOS_KernelAllocator->ZeroAllocate(OBOS_KernelAllocator, valuelen+1, sizeof(char), nullptr), arg, valuelen);
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
	return temp > 0 && temp < 10;
}
static uint64_t strtoull(const char* str, char** endptr, int base)
{
	while (!isNumber(*str++));
	if (!base)
	{
		base = 10;
		if (*(str - 1) == 'x' || *(str - 1) == 'X')
			base = 16;
		else if (*str == '0')
		{
			base = 8;
			str++;
		}
	}
	size_t sz = 0;
	while (isNumber(*str++))
		sz++;
	if (endptr)
		*endptr = (char*)(str + sz);
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
uint64_t OBOS_GetOPTD(const char* opt)
{
    char* val = OBOS_GetOPTS(opt);
    if (!val)
        return 0;
    uint64_t dec = strtoull(val, nullptr, 0);
    size_t val_len = 0;
    OBOS_KernelAllocator->QueryBlockSize(OBOS_KernelAllocator, val, &val_len);
    OBOS_KernelAllocator->Free(OBOS_KernelAllocator, val, val_len);
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
        if (uacpi_strncmp(opt, arg, optlen) == 0)
            return true;
    }
    return false;
}