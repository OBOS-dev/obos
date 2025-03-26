/*
 * oboskrnl/text.c
 *
 * Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <klog.h>
#include <text.h>
#include <memmanip.h>
#include <error.h>

// Makeshift text renderer.

text_renderer_state OBOS_TextRendererState;

// colour is rgbx.
__attribute__((no_instrument_function)) void OBOS_PlotPixel(uint32_t colour, uint8_t* fb, uint16_t fbFmt)
{
	switch (fbFmt)
	{
	case OBOS_FB_FORMAT_RGB888:
		*fb++ = colour & 0xff;
		*fb++ = (colour >> 8) & 0xff;
		*fb++ = (colour >> 16) & 0xff;
		break;
	case OBOS_FB_FORMAT_BGR888:
		*fb++ = (colour >> 16) & 0xff;
		*fb++ = (colour >> 8) & 0xff;
		*fb++ = colour & 0xff;
		break;
	case OBOS_FB_FORMAT_RGBX8888:
		*(uint32_t*)fb = colour;
		break;
	case OBOS_FB_FORMAT_XRGB8888:
		colour >>= 8;
		*(uint32_t*)fb = colour;
		break;
	default:
		break;
	}
}
static OBOS_NO_UBSAN __attribute__((no_instrument_function)) void putch(text_renderer_state* state, char ch, uint32_t x, uint32_t y, uint32_t fc, uint32_t bc)
{
	int cy;
	static const int mask[8] = { 128,64,32,16,8,4,2,1 };
	const uint8_t* glyph = (uint8_t*)state->font + (int)ch * 16;
	y = y * 16 + 12;
	x <<= 3;
	if (x > state->fb.width)
		x = 0;
	if (y >= state->fb.height)
		y = state->fb.height - 16;
	const uint32_t bytesPerPixel = state->fb.bpp / 8;
	void* fb = state->fb.backbuffer_base ? state->fb.backbuffer_base : state->fb.base;
	for (cy = 0; cy < 16; cy++)
	{
		uint32_t realY = y + cy - 12;
		uint8_t* framebuffer = (uint8_t*)fb + realY * state->fb.pitch;
		OBOS_PlotPixel((glyph[cy] & mask[0]) ? fc : bc, framebuffer + ((x + 0) * bytesPerPixel), state->fb.format);
		OBOS_PlotPixel((glyph[cy] & mask[1]) ? fc : bc, framebuffer + ((x + 1) * bytesPerPixel), state->fb.format);
		OBOS_PlotPixel((glyph[cy] & mask[2]) ? fc : bc, framebuffer + ((x + 2) * bytesPerPixel), state->fb.format);
		OBOS_PlotPixel((glyph[cy] & mask[3]) ? fc : bc, framebuffer + ((x + 3) * bytesPerPixel), state->fb.format);
		OBOS_PlotPixel((glyph[cy] & mask[4]) ? fc : bc, framebuffer + ((x + 4) * bytesPerPixel), state->fb.format);
		OBOS_PlotPixel((glyph[cy] & mask[5]) ? fc : bc, framebuffer + ((x + 5) * bytesPerPixel), state->fb.format);
		OBOS_PlotPixel((glyph[cy] & mask[6]) ? fc : bc, framebuffer + ((x + 6) * bytesPerPixel), state->fb.format);
		OBOS_PlotPixel((glyph[cy] & mask[7]) ? fc : bc, framebuffer + ((x + 7) * bytesPerPixel), state->fb.format);
	}
}
void OBOS_FlushBuffers(text_renderer_state* state)
{
	if (!state->fb.backbuffer_base)
		return;
	uintptr_t currentLine = (uintptr_t)state->fb.base;
	uintptr_t currentLineBackbuffer = (uintptr_t)state->fb.backbuffer_base;
	for (size_t j = 0; j < get_line_bitmap_size(state->fb.height); j++)
	{
		int limit = 32;
		if (j == (get_line_bitmap_size(state->fb.height) - 1))
			limit = (state->fb.height / 16) % 32;
		for (int i = 0; i < limit; i++)
		{
			bool wasModified = state->fb.modified_line_bitmap[j] & BIT(i);
			if (wasModified)
				memcpy((void*)currentLine, (void*)currentLineBackbuffer, state->fb.pitch*16);
			currentLine += state->fb.pitch*16;
			currentLineBackbuffer += state->fb.pitch*16;
			state->fb.modified_line_bitmap[j] &= ~BIT(i);
		}
	}
}
static void newlineHandler(text_renderer_state* state)
{
	if (!state->fb.base)
		return;
	state->column = 0;
	if (++state->row == (state->fb.height / 16))
	{
		void* fb = state->fb.backbuffer_base ? state->fb.backbuffer_base : state->fb.base;
		memset(fb, 0, (size_t)state->fb.pitch / 4 * 16);
		memcpy(fb, (uint8_t*)fb + (state->fb.pitch * 16), (size_t)state->fb.pitch * ((size_t)state->fb.height - 16));
		if (OBOS_TEXT_BACKGROUND == 0)
			memset((uint8_t*)fb + (size_t)state->fb.pitch * ((size_t)state->fb.height - 16), 0, (size_t)state->fb.pitch * 16);
		else {
			uint32_t y = state->fb.height - 16;
			for (size_t i = 0; i < 16; i++, y++)
				for (uint32_t x = 0; x < state->fb.width; x++)
					OBOS_PlotPixel(OBOS_TEXT_BACKGROUND, &((uint8_t*)fb)[y*state->fb.pitch+x*state->fb.bpp/8], state->fb.format);
		}
		state->row--;
		if (state->fb.modified_line_bitmap)
		{
			memset(state->fb.modified_line_bitmap, 0xff, get_line_bitmap_size(state->fb.height)*4);
			state->fb.modified_line_bitmap[get_line_bitmap_size(state->fb.height)-1] &= ~BIT(31);
		}
	}
	OBOS_FlushBuffers(state);
}
obos_status OBOS_WriteCharacter(text_renderer_state* state, char ch)
{
	if (!state->fb.base)
		return OBOS_STATUS_INVALID_INIT_PHASE;
	if (ch < 0x20 && ch != '\r' && ch != '\n' && ch != '\t' && ch != '\b')
	{
		OBOS_WriteCharacter(state, '^');
		OBOS_WriteCharacter(state, ch+0x40);
		return OBOS_STATUS_SUCCESS;
	}
	switch (ch)
	{
	case '\n':
		newlineHandler(state);
		break;
	case '\r':
		state->column = 0;
		break;
	case '\t':
		state->column += 4 - (state->column % 4);
		break;
	case '\b':
	case '\177':
		if (!state->column)
			break;
		putch(state, ' ', --state->column, state->row, state->fg_color, OBOS_TEXT_BACKGROUND);
		break;
	default:
		if (state->column >= (state->fb.width/8))
			newlineHandler(state);
		putch(state, ch, state->column++, state->row, state->fg_color, OBOS_TEXT_BACKGROUND);
		break;
	}
	if (state->fb.backbuffer_base)
	{
		OBOS_ASSERT(state->fb.modified_line_bitmap);
		state->fb.modified_line_bitmap[state->row / 32] |= BIT(state->row % 32);
	}
	return OBOS_STATUS_SUCCESS;
}
obos_status OBOS_WriteCharacterAt(text_renderer_state* state, char ch, uint32_t column, uint32_t row)
{
	if (!state->fb.base)
		return OBOS_STATUS_INVALID_INIT_PHASE;
	if (column >= (state->fb.width / 8))
		return OBOS_STATUS_INVALID_ARGUMENT;
	if (row >= (state->fb.height / 16))
		return OBOS_STATUS_INVALID_ARGUMENT;
	switch (ch)
	{
	case '\n':
		break;
	case '\r':
		break;
	case '\t':
		break;
	case '\b':
		break;
	default:
		putch(state, ch, column, row, state->fg_color, OBOS_TEXT_BACKGROUND);
		break;
	}
	if (state->fb.backbuffer_base)
	{
		OBOS_ASSERT(state->fb.modified_line_bitmap);
		state->fb.modified_line_bitmap[row / 32] |= BIT(row % 32);
	}
	return OBOS_STATUS_SUCCESS;
}
