/*
 * oboskrnl/text.c
 *
 * Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <text.h>
#include <memmanip.h>

// Makeshift text renderer.

text_renderer_state OBOS_TextRendererState;

// colour is rgbx.
static void PlotPixel(uint32_t colour, uint8_t* fb, uint16_t fbFmt)
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
static void putch(text_renderer_state* state, char ch, uint32_t x, uint32_t y, uint32_t fc, uint32_t bc)
{
	int cy;
	int mask[8] = { 128,64,32,16,8,4,2,1 };
	const uint8_t* glyph = (uint8_t*)state->font + (int)ch * 16;
	y = y * 16 + 12;
	x <<= 3;
	if (x > state->fb.width)
		x = 0;
	if (y >= state->fb.height)
		y = state->fb.height - 16;
	const uint32_t bytesPerPixel = state->fb.bpp / 8;
	for (cy = 0; cy < 16; cy++)
	{
		uint32_t realY = y + cy - 12;
		uint8_t* framebuffer = (uint8_t*)state->fb.base + realY * state->fb.pitch;
		PlotPixel((glyph[cy] & mask[0]) ? fc : bc, framebuffer + ((x + 0) * bytesPerPixel), state->fb.format);
		PlotPixel((glyph[cy] & mask[1]) ? fc : bc, framebuffer + ((x + 1) * bytesPerPixel), state->fb.format);
		PlotPixel((glyph[cy] & mask[2]) ? fc : bc, framebuffer + ((x + 2) * bytesPerPixel), state->fb.format);
		PlotPixel((glyph[cy] & mask[3]) ? fc : bc, framebuffer + ((x + 3) * bytesPerPixel), state->fb.format);
		PlotPixel((glyph[cy] & mask[4]) ? fc : bc, framebuffer + ((x + 4) * bytesPerPixel), state->fb.format);
		PlotPixel((glyph[cy] & mask[5]) ? fc : bc, framebuffer + ((x + 5) * bytesPerPixel), state->fb.format);
		PlotPixel((glyph[cy] & mask[6]) ? fc : bc, framebuffer + ((x + 6) * bytesPerPixel), state->fb.format);
		PlotPixel((glyph[cy] & mask[7]) ? fc : bc, framebuffer + ((x + 7) * bytesPerPixel), state->fb.format);
	}
}
static void newlineHandler(text_renderer_state* state)
{
	if (!state->fb.base)
		return;
	state->column = 0;
	if (++state->row == (state->fb.height / 16))
	{
		memset(state->fb.base, 0, (size_t)state->fb.pitch / 4 * 16);
		memcpy(state->fb.base, (uint8_t*)state->fb.base + (state->fb.pitch * 16), (size_t)state->fb.pitch * ((size_t)state->fb.height - 16));
		memset((uint8_t*)state->fb.base + (size_t)state->fb.pitch * ((size_t)state->fb.height - 16), 0, (size_t)state->fb.pitch * 16);
		state->row--;
	}
}
obos_status OBOS_WriteCharacter(text_renderer_state* state, char ch)
{
	if (!state->fb.base)
		return OBOS_STATUS_INVALID_INIT_PHASE;
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
		if (!state->column)
			break;
		putch(state, ' ', --state->column, state->row, 0xcccccccc, 0);
		break;
	default:
		if (state->column >= (state->fb.width/8))
			newlineHandler(state);
		putch(state, ch, state->column++, state->row, 0xcccccccc, 0);
		break;
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
		putch(state, ch, column, row, 0xcccccccc, 0);
		break;
	}
	return OBOS_STATUS_SUCCESS;
}