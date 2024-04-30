/*
	oboskrnl/error.h

	Copyright (c) 2024 Omar Berrow
*/

#pragma once

typedef enum
{
	/// <summary>
	/// The function succeeded.
	/// </summary>
	OBOS_STATUS_SUCCESS,
	/// <summary>
	/// An invalid IRQL value was passed to the function.
	/// </summary>
	OBOS_STATUS_INVALID_IRQL,
	/// <summary>
	/// An invalid argument was passed to the function.
	/// </summary>
	OBOS_STATUS_INVALID_ARGUMENT,
	/// <summary>
	/// A function was called, but an option passed to it had not been implemented yet.
	/// </summary>
	OBOS_STATUS_UNIMPLEMENTED,
} obos_status;