/*
	oboskrnl/arch/x86_64/kdbg/io.cpp

	Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <console.h>

#include <arch/x86_64/kdbg/io.h>

#include <arch/x86_64/asm_helpers.h>

#define STB_SPRINTF_NOFLOAT 1
#define STB_SPRINTF_MIN 8
#include <external/stb_sprintf.h>

#include <locks/spinlock.h>

#include <allocators/allocator.h>

namespace obos
{
	namespace kdbg
	{
		output_format g_outputDev;
		input_format g_inputDev;
#if OBOS_KDBG_ENABLED
		static bool s_com1Initialized = false;
		static bool s_ps2kInitialized = false;
		constexpr uint16_t COM1 = 0x3F8;
		static void common_initialize_serial(const uint16_t port)
		{
			constexpr uint8_t EIGHT_DATABITS = 0b11;
			constexpr uint8_t ONE_STOPBIT = 0b0;
			constexpr uint8_t PARITY_EVEN = 0b11000;
			outb(port + 1, 0x01);
			// Enable DLAB
			outb(port + 3, 0x80);
			// Set the baud rate divisor to 1 (115200 ticks).
			outb(port, 1);
			outb(port + 1, 0);
			// Set the data bits, stop bits, and parity bit
			outb(port + 3, EIGHT_DATABITS | ONE_STOPBIT | PARITY_EVEN);
		
			// RTS/DSR are set.
			outb(port + 4, 0x03);
			// Set the serial chip to "loopback mode."
			outb(port + 4, 0x1E);
		
			// Check if the serial port is working correctly. 
			outb(port, 0xAE);
			if (inb(port) != 0xAE)
				return;
			
			// Disable all interrupts for the serial port.
			outb(port + 2, 0x00);
		
			// Exit loopback mode.
			outb(port + 4, 0x03);
		}
		static char getchar_serial(bool async)
		{
			if (!s_com1Initialized)
			{
				common_initialize_serial(COM1);
				s_com1Initialized = true;
			}
			if (!(inb(COM1 + 5) & (1<<0)) && async)
				return EOF;
			while (!(inb(COM1 + 5) & (1<<0)))
				pause();
			return inb(COM1);
		}
		static uint8_t ps2k_sendCommand(uint32_t nCommands, ...);
		static void common_initialize_ps2k()
		{
			// Keys need to be held for 250 ms before repeating, and they repeat at a rate of 30 hz (33.33333 ms).
			ps2k_sendCommand(2, 0xF3, 0);
		
			// Set scancode set 1.
			ps2k_sendCommand(2, 0xF0, 0);
		
			// Enable scanning.
			ps2k_sendCommand(1, 0xF4);
		
			// Clear all keyboard LEDs.
			ps2k_sendCommand(2, 0xED, 0b000);
		}
		static uint8_t ps2k_sendCommand(uint32_t nCommands, ...)
		{
			uint8_t ret = 0x00;
			va_list list;
			uint8_t *commands = new uint8_t[nCommands];
			va_start(list, nCommands);
			for (uint32_t x = 0; x < nCommands; x++)
				commands[x] = va_arg(list, int);
			va_end(list);
			for (int i = 0; i < 5; i++)
			{
				for (size_t x = 0; x < nCommands; x++)
				{
					uint8_t& c = commands[x];
					while ((inb(0x64) & 0b10) == 0b10);
					outb(0x60, c);
				}
				ret = inb(0x60);
				if (ret == 0xFA /*ACK*/)
					break;
				if (ret == 0xFE /*RESEND*/)
					continue;
				// Assume abort.
				break;
			}
			delete[] commands;
			return ret;
		}
		static bool isNumber(char ch)
		{
			char temp = ch - '0';
			return temp >= 0 && temp < 10;
		}
		static bool isAlpha(char ch)
		{
			char tmp1 = ch - 'A', tmp2 = ch - 'a';
			return (tmp1 > 0 && tmp1 < 26) || (tmp2 > 0 && tmp2 < 26);
		}
		static size_t getLetterIndex(char ch)
		{
			size_t tmp1 = ch - 'A', tmp2 = ch - 'a';
			if (tmp1 > 0 && tmp1 < 26)
				return tmp1;
			if (tmp2 > 0 && tmp2 < 26)
				return tmp2;
			return SIZE_MAX;
		}
		static char getchar_keyboard(bool async)
		{
			if (!s_ps2kInitialized)
			{
				common_initialize_ps2k();
				s_ps2kInitialized = true;
			}
			static struct
			{
				bool capsLock : 1;
				bool shiftPressed : 1;
			} flags;
			static char scancode_table[] = 
			{
				/*0x00*/ '\0'  , /*0x01*/ '\x1f', /*0x02*/ '1' , /*0x03*/ '2' ,
				/*0x04*/ '3'   , /*0x05*/ '4'   , /*0x06*/ '5' , /*0x07*/ '6' ,
				/*0x08*/ '7'   , /*0x09*/ '8'   , /*0x0A*/ '9' , /*0x0B*/ '0' ,
				/*0x0C*/ '-'   , /*0x0D*/ '='   , /*0x0E*/ '\b', /*0x0F*/ '\t',
				/*0x10*/ 'q'   , /*0x11*/ 'w'   , /*0x12*/ 'e' , /*0x13*/ 'r' ,
				/*0x14*/ 't'   , /*0x15*/ 'y'   , /*0x16*/ 'u' , /*0x17*/ 'i' ,
				/*0x18*/ 'o'   , /*0x19*/ 'p'   , /*0x1A*/ '[' , /*0x1B*/ ']' ,
				/*0x1C*/ '\n'  , /*0x1D*/ '\0'  , /*0x1E*/ 'a' , /*0x1F*/ 's' ,
				/*0x20*/ 'd'   , /*0x21*/ 'f'   , /*0x22*/ 'g' , /*0x23*/ 'h' ,
				/*0x24*/ 'j'   , /*0x25*/ 'k'   , /*0x26*/ 'l' , /*0x27*/ ';' ,
				/*0x28*/ '\''  , /*0x29*/ '`'   , /*0x2A*/ '\0', /*0x2B*/ '\\',
				/*0x2C*/ 'z'   , /*0x2D*/ 'x'   , /*0x2E*/ 'c' , /*0x2F*/ 'v' ,
				/*0x30*/ 'b'   , /*0x31*/ 'n'   , /*0x32*/ 'm' , /*0x33*/ ',' ,
				/*0x34*/ '.'   , /*0x35*/ '/'   , /*0x36*/ '\0', /*0x37*/ '*' ,
				/*0x38*/ '\0'  , /*0x39*/ ' '   , /*0x3A*/ '\0', /*0x3B*/ '\0',
				/*0x3C*/ '\0'  , /*0x3D*/ '\0'  , /*0x3E*/ '\0', /*0x3F*/ '\0',
				/*0x40*/ '\0'  , /*0x41*/ '\0'  , /*0x42*/ '\0', /*0x43*/ '\0',
				/*0x44*/ '\0'  , /*0x45*/ '\0'  , /*0x46*/ '\0', /*0x47*/ '7' , 
				/*0x48*/ '8'   , /*0x49*/ '9'   , /*0x4A*/ '-' , /*0x4B*/ '4' ,
				/*0x4C*/ '5'   , /*0x4D*/ '6'   , /*0x4E*/ '+' , /*0x4F*/ '1' ,
				/*0x50*/ '2'   , /*0x51*/ '3'   , /*0x52*/ '0' , /*0x53*/ '.' ,
			};                                                 
			while (1)
			{
				if (async && !(inb(0x64) & 1<<0))
					return EOF;
				while (!(inb(0x64) & 1<<0))
					pause();
				uint8_t scancode = inb(0x60);
				bool wasRelease = scancode & 0x80;
				scancode &= ~0x80;
				char ch = 0;
				ch = wasRelease ? EOF : scancode_table[scancode];
				// If shift was pressed.
				if (scancode == 0x2A || scancode == 0x36)
					flags.shiftPressed = !wasRelease;
				if (scancode == 0x3A)
					flags.capsLock = !flags.capsLock;
				if (!async && !ch)
					continue;
				if (flags.shiftPressed)
				{
					const char *shiftAlias = "ABCDEFGHIJKLMNOPQRSTUVWXYZ" "!@#$%^&*()" "<>?:\"{}|_+~";
					size_t index = SIZE_MAX;
					if (isAlpha(ch))
						index = getLetterIndex(ch);
					else if (isNumber(ch))
						index = (ch - '0') + 25 + (ch == '0' ? 10 : 0);
					else
					{
						switch(ch)
						{
						case ',' : index = 36+0; break;
						case '.' : index = 36+1; break;
						case '/' : index = 36+2; break;
						case ';' : index = 36+3; break;
						case '\'': index = 36+4; break;
						case '[' : index = 36+5; break;
						case ']' : index = 36+6; break;
						case '\\': index = 36+7; break;
						case '-' : index = 36+8; break;
						case '=' : index = 36+9; break;
						case '`' : index = 36+10; break;
						default: break;
						}
					}
					if (index != SIZE_MAX)
						ch = shiftAlias[index];
				}
				if (flags.capsLock)
				{
					const char *capsAlias = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
					if (isAlpha(ch))
						ch = capsAlias[ch-'a'];
				}
				return ch == 0 ? EOF : ch;
			}
		}
		static bool putchar_console(char ch, bool)
		{
			g_kernelConsole.ConsoleOutput(ch);
			return true;
		}
		static bool putchar_serial(char ch, bool async)
		{
			if (!s_com1Initialized)
			{
				common_initialize_serial(COM1);
				s_com1Initialized = true;
			}
			if (!(inb(COM1 + 5) & (1<<5)) && async)
				return false;
			while (!(inb(COM1 + 5) & (1<<5)))
				pause();
			outb(COM1, ch);
			return true;
		}
		static char(*s_inputHandlers[2])(bool) = { getchar_keyboard, getchar_serial };
		static bool(*s_outputHandlers[2])(char, bool) = { putchar_console, putchar_serial };
		char getchar(bool async, bool echo)
		{
			if ((int)g_inputDev > 2 || (int)g_inputDev < 0)
				return EOF;
			char ch = s_inputHandlers[(int)g_inputDev - 1](async);
			if (echo && ch != EOF)
				putchar(ch, async);
			return ch;
		}
		bool putchar(char ch, bool async)
		{
			if ((int)g_outputDev > 2 || (int)g_outputDev < 0)
				return false;
			if (ch == '\n')
			{
				bool r1 = s_outputHandlers[(int)g_outputDev - 1]('\r', async);
				if (!r1)
					return false;
				bool r2 = s_outputHandlers[(int)g_outputDev - 1]('\n', async);
				return r2;
			}
			return s_outputHandlers[(int)g_outputDev - 1](ch, async);
		}
		static char* outputCallback(const char* buf, void*, int len)
		{
			for (size_t i = 0; i < len; i++)
				putchar(buf[i], false);
			return (char*)buf;
		}
		size_t printf(const char* format, ...)
		{
			static locks::SpinLock kdbg_printf_lock;
			kdbg_printf_lock.Lock();
			va_list list;
			va_start(list, format);
			char ch[8];
			size_t ret = stbsp_vsprintfcb(outputCallback, nullptr, ch, format, list);
			va_end(list);
			kdbg_printf_lock.Unlock();
			return ret;
		}
		char* getline()
		{
			char *ret = nullptr;
			size_t stringSize = 0;
			char ch = 0;
			while((ch = getchar(false, false)) != '\n')
			{
				if (ch == EOF)
					continue;
				if (ch != '\b')
				{
					ret = (char*)allocators::g_kAllocator->ReAllocate(ret, ++stringSize + 1);
					ret[stringSize - 1] = ch;
					putchar(ch);
				}
				else
				{
					if (stringSize != 0) 
					{
						ret = (char*)allocators::g_kAllocator->ReAllocate(ret, --stringSize);
						putchar(ch);
					}
					else
						continue;
				}	
				if (ret)
					ret[stringSize] = 0;
			}
			putchar('\n');
			return ret;
		}
#else
		char getchar(bool async, bool echo)
		{
			return EOF;
		}
		bool putchar(char ch, bool async)
		{
			return false;
		}
		size_t printf(const char* format, ...)
		{
			return 0;
		}
		char* getline()
		{
			return nullptr;
		}
#endif
	}
}