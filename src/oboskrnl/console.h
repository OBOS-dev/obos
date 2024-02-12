/*
	oboskrnl/console.h

	Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>
#include <fb.h>

#include <locks/spinlock.h>

namespace obos
{
	class Console
	{
	public:
		Console() = default;

		/// <summary>
		/// Initializes the console.<para></para>
		/// If both buffers are nullptr, the draw buffer is set to nullptr and all writes to the console are dropped.
		/// </summary>
		/// <param name="fb">The framebuffer to use.</param>
		/// <param name="bb">The backbuffer to use.</param>
		/// <param name="drawToFB">Whether to set the draw buffer to the framebuffer (true) or the backbuffer (false). If the framebuffer is nullptr and this is set, 
		/// the parameter is treated as if it were false. If the backbuffer is nullptr and this is false, the parameter is treated as if it were true.</param>
		/// <returns></returns>
		bool Initialize(const Framebuffer *fb, const Framebuffer *bb, bool drawToFB);

		/// <summary>
		/// Prints a string.
		/// </summary>
		/// <param name="string">The string to print.</param>
		void ConsoleOutput(const char* string);
		/// <summary>
		/// Prints a string.
		/// </summary>
		/// <param name="string">The string to print.</param>
		/// <param name="size">The amount of characters to print.</param>
		void ConsoleOutput(const char* string, size_t size);
		/// <summary>
		/// Prints a single character.
		/// </summary>
		/// <param name="ch">The character to print.</param>
		void ConsoleOutput(char ch);

		void SetColour(Pixel fg, Pixel bg, bool fillBg = false);
		void GetColour(Pixel &fg, Pixel &bg) const;
		void SetPosition(uint32_t x, uint32_t y);
		void GetPosition(uint32_t* x, uint32_t* y) const;

		void ClearConsole(Pixel bg);
	private:
		void __ImplPutChar(char ch, uint32_t x, uint32_t y, Pixel fc, Pixel bc);
		void __ImplOutputCharacter(char ch);
		void newlineHandler();
		Framebuffer* m_drawBuffer;
		Framebuffer  m_framebuffer;
		Framebuffer  m_backbuffer;
		uint32_t m_x, m_y;
		uint32_t m_maxX = 0, m_maxY = 0;
		Pixel m_foregroundColour{ 0xCC, 0xCC, 0xCC };
		Pixel m_backgroundColour{ 0 };
		locks::SpinLock m_lock;
	};

	// Must be an 8x16 font.
	extern void* g_consoleFont;
	extern Console g_kernelConsole;
}