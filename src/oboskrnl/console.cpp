/*
	oboskrnl/console.cpp

	Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <console.h>
#include <fb.h>
#include <memmanip.h>

namespace obos
{
	static void PlotPixel(Pixel colour, uint8_t* fb, FramebufferFormat fbFmt);
	bool Console::Initialize(const Framebuffer* fb, const Framebuffer* bb, bool drawToFB)
	{
		SetFramebuffer(fb, bb, drawToFB);
		return true;
	}

	void Console::ConsoleOutput(const char* string)
	{
		m_lock.Lock();
		for (size_t i = 0; string[i]; __ImplOutputCharacter(string[i++]));
		m_lock.Unlock();
	}
	void Console::ConsoleOutput(const char* string, size_t size)
	{
		m_lock.Lock();
		for (size_t i = 0; i < size; __ImplOutputCharacter(string[i++]));
		m_lock.Unlock();
	}
	void Console::ConsoleOutput(char ch)
	{
		m_lock.Lock();
		__ImplOutputCharacter(ch);
		m_lock.Unlock();
	}

	static void fillBackgroundTransparent(Framebuffer& framebuffer, Pixel backgroundColour, Pixel initialBackgroundColour)
	{
		for (uint32_t i = 0; i < framebuffer.height * framebuffer.width * framebuffer.bpp / 8; i += (framebuffer.bpp / 8))
		{
			Pixel pixel = 0;
			switch (framebuffer.format)
			{
			case FramebufferFormat::FB_FORMAT_RGB888:
				pixel.ele.r = ((uint8_t*)framebuffer.address)[i + 0];
				pixel.ele.g = ((uint8_t*)framebuffer.address)[i + 1];
				pixel.ele.b = ((uint8_t*)framebuffer.address)[i + 2];
				break;
			case FramebufferFormat::FB_FORMAT_BGR888:
				pixel.ele.b = ((uint8_t*)framebuffer.address)[i + 0];
				pixel.ele.g = ((uint8_t*)framebuffer.address)[i + 1];
				pixel.ele.r = ((uint8_t*)framebuffer.address)[i + 2];
				break;
			case FramebufferFormat::FB_FORMAT_RGBX8888:
				pixel.raw.rgbx = *(uint32_t*)((uint8_t*)framebuffer.address + i);
				break;
			case FramebufferFormat::FB_FORMAT_XRGB8888:
				pixel.ele.r = ((uint8_t*)framebuffer.address)[i + 1];
				pixel.ele.g = ((uint8_t*)framebuffer.address)[i + 2];
				pixel.ele.b = ((uint8_t*)framebuffer.address)[i + 3];
				break;
			default:
				break;
			}
			PlotPixel(pixel == initialBackgroundColour ? backgroundColour : pixel, ((uint8_t*)framebuffer.address) + i, framebuffer.format);
		}
	}
	void Console::SetColour(Pixel fg, Pixel bg, bool fillBg)
	{
		if (m_backgroundColour != bg && fillBg)
			fillBackgroundTransparent(*m_drawBuffer, bg, m_backgroundColour);
		m_foregroundColour = fg;
		m_backgroundColour = bg;
	}
	void Console::GetColour(Pixel& fg, Pixel& bg) const
	{
		fg = m_foregroundColour;
		bg = m_backgroundColour;
	}
	void Console::SetPosition(uint32_t x, uint32_t y)
	{
		x = x % m_maxX;
		y = y % m_maxY;
		m_x = x;
		m_y = y;
	}
	void Console::GetPosition(uint32_t* x, uint32_t* y) const
	{
		if (x)
			*x = m_x;
		if (y)
			*y = m_y;
	}
	void Console::SetFramebuffer(const Framebuffer* fb, const Framebuffer* bb, bool drawToFB)
	{
		if (fb)
			m_framebuffer = *fb;
		if (bb)
			m_backbuffer = *bb;
		if (drawToFB && !fb)
			drawToFB = false;
		if (!drawToFB && !bb)
			drawToFB = true;
		if (drawToFB)
			m_drawBuffer = &m_framebuffer;
		else
			m_drawBuffer = &m_backbuffer;
		if (m_drawBuffer)
		{
			m_maxX = m_drawBuffer->width / 8;
			m_maxY = m_drawBuffer->height / 16;
		}
	}
	void Console::GetFramebuffer(Framebuffer* fb, Framebuffer* bb, bool* drawToFB)
	{
		if (fb)
			*fb = m_framebuffer;
		if (bb)
			*bb = m_backbuffer;
		if (drawToFB)
			*drawToFB = (m_drawBuffer == &m_framebuffer);
	}

	void Console::ClearConsole(Pixel bg)
	{
		if (!m_drawBuffer)
			return;
		for (uint32_t i = 0; i < m_drawBuffer->height * m_drawBuffer->pitch; i += (m_drawBuffer->bpp / 8))
			PlotPixel(bg, ((uint8_t*)m_drawBuffer->address) + i, m_drawBuffer->format);
	}

	void Console::__ImplOutputCharacter(char ch)
	{
		switch (ch)
		{
		case '\n':
			newlineHandler();
			break;
		case '\r':
			m_x = 0;
			break;
		case '\t':
			m_x += 4 - (m_x % 4);
			break;
		case '\b':
			if (!m_x)
				break;
			__ImplPutChar(' ', --m_x, m_y, m_foregroundColour, m_backgroundColour);
			break;
		default:
			if (m_x >= m_maxX)
				newlineHandler();
			__ImplPutChar(ch, m_x++, m_y, m_foregroundColour, m_backgroundColour);
			break;
		}
	}
	void Console::newlineHandler()
	{
		m_x = 0;
		if (++m_y == m_maxY)
		{
			memset(m_drawBuffer->address, 0, (size_t)m_drawBuffer->pitch / 4 * 16);
			memcpy(m_drawBuffer->address, (uint8_t*)m_drawBuffer->address + (m_drawBuffer->pitch * 16), (size_t)m_drawBuffer->pitch * ((size_t)m_drawBuffer->height - 16));
			memset((uint8_t*)m_drawBuffer->address + (size_t)m_drawBuffer->pitch * ((size_t)m_drawBuffer->height - 16), 0, (size_t)m_drawBuffer->pitch * 16);
			m_y--;
		}
	}
	static void PlotPixel(Pixel colour, uint8_t* fb, FramebufferFormat fbFmt)
	{
		switch (fbFmt)
		{
		case obos::FramebufferFormat::FB_FORMAT_RGB888:
			*fb++ = colour.ele.r;
			*fb++ = colour.ele.g;
			*fb++ = colour.ele.b;
			break;
		case obos::FramebufferFormat::FB_FORMAT_BGR888:
			*fb++ = colour.ele.b;
			*fb++ = colour.ele.g;
			*fb++ = colour.ele.r;
			break;
		case obos::FramebufferFormat::FB_FORMAT_RGBX8888:
			*(uint32_t*)fb = colour.raw.rgbx;
			break;
		case obos::FramebufferFormat::FB_FORMAT_XRGB8888:
			*(uint32_t*)fb = colour.ToFormat(FramebufferFormat::FB_FORMAT_XRGB8888);
			break;
		default:
			break;
		}
	}
	void Console::__ImplPutChar(char ch, uint32_t x, uint32_t y, Pixel fc, Pixel bc)
	{
		if (!m_drawBuffer)
			return;
		int cy;
		int mask[8] = { 128,64,32,16,8,4,2,1 };
		const uint8_t* glyph = (uint8_t*)g_consoleFont + (int)ch * 16;
		y = y * 16 + 12;
		x <<= 3;
		if (x > m_drawBuffer->width)
			x = 0;
		if (y >= m_drawBuffer->height)
			y = m_drawBuffer->height - 16;
		const uint32_t bytesPerPixel = m_drawBuffer->bpp / 8;
		for (cy = 0; cy < 16; cy++)
		{
			uint32_t realY = y + cy - 12;
			uint8_t* framebuffer = (uint8_t*)m_drawBuffer->address + realY * m_drawBuffer->pitch;
			PlotPixel((glyph[cy] & mask[0]) ? fc : bc, framebuffer + ((x + 0) * bytesPerPixel), m_drawBuffer->format);
			PlotPixel((glyph[cy] & mask[1]) ? fc : bc, framebuffer + ((x + 1) * bytesPerPixel), m_drawBuffer->format);
			PlotPixel((glyph[cy] & mask[2]) ? fc : bc, framebuffer + ((x + 2) * bytesPerPixel), m_drawBuffer->format);
			PlotPixel((glyph[cy] & mask[3]) ? fc : bc, framebuffer + ((x + 3) * bytesPerPixel), m_drawBuffer->format);
			PlotPixel((glyph[cy] & mask[4]) ? fc : bc, framebuffer + ((x + 4) * bytesPerPixel), m_drawBuffer->format);
			PlotPixel((glyph[cy] & mask[5]) ? fc : bc, framebuffer + ((x + 5) * bytesPerPixel), m_drawBuffer->format);
			PlotPixel((glyph[cy] & mask[6]) ? fc : bc, framebuffer + ((x + 6) * bytesPerPixel), m_drawBuffer->format);
			PlotPixel((glyph[cy] & mask[7]) ? fc : bc, framebuffer + ((x + 7) * bytesPerPixel), m_drawBuffer->format);
		}
	}

	void* g_consoleFont;
	Console g_kernelConsole;
}