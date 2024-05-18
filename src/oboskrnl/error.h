/*
 * oboskrnl/error.h
 * 
 * Copyright (c) 2024 Omar Berrow
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
	/// <summary>
	/// A function was called before one of the prerequisite components was initialized.
	/// </summary>
	OBOS_STATUS_INVALID_INIT_PHASE,
	/// <summary>
	/// The affinity in the thread object is invalid.
	/// </summary>
	OBOS_STATUS_INVALID_AFFINITY,
	/// <summary>
	/// There is not enough memory to satisfy your request.
	/// </summary>
	OBOS_STATUS_NOT_ENOUGH_MEMORY,
	/// <summary>
	/// A mismatched pointer was passed.
	/// </summary>
	OBOS_STATUS_MISMATCH,
	/// <summary>
	/// An internal error happened.
	/// </summary>
	OBOS_STATUS_INTERNAL_ERROR,
	/// <summary>
	/// An error occurred, and a retry of the operation is needed.
	/// </summary>
	OBOS_STATUS_RETRY,
	/// <summary>
	/// The object is already initialized.
	/// </summary>
	OBOS_STATUS_ALREADY_INITIALIZED,
	/// <summary>
	/// The request could not be fulfilled, as a required resource was not found.
	/// </summary>
	OBOS_STATUS_NOT_FOUND,
	/// <summary>
	/// The resource is already in use.
	/// </summary>
	OBOS_STATUS_IN_USE,
} obos_status;