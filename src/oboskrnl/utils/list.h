/*
 * oboskrnl/utils/list.h
 *
 * Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>

#define LIST_HEAD(name, type) \
struct name\
{\
    type *head, *tail;\
    size_t nNodes;\
}

#define LIST_NODE(name, type) LIST_NODE_INTERNAL(name, type, )
#define LIST_NODE_INTERNAL(name, type, attrib) \
attrib struct name##_list_node \
{\
    type *next, *prev;\
}

#define LIST_INIT(name) (name)({ nullptr, nullptr, 0 })

// TODO: add more functions here

#define LIST_PROTOTYPE(name, type, field) \
LIST_PROTOTYPE_INTERNAL(name, type, field, )
#define LIST_PROTOTYPE_STATIC(name, type, field) \
LIST_PROTOTYPE_INTERNAL(name, type, field, static)
#define LIST_PROTOTYPE_INTERNAL(name, type, field, attrib) \
attrib void name##_LIST_APPEND(name* list, type* what);\
attrib void name##_LIST_PREPEND(name* list, type* what);\
attrib void name##_LIST_REMOVE(name* list, type* what);\
attrib type* name##_LIST_GET_NEXT(name* list, type* what);\
attrib type* name##_LIST_GET_PREV(name* list, type* what)

#define LIST_GENERATE(name, type, field) \
LIST_GENERATE_INTERNAL(name, type, field, )
#define LIST_GENERATE_STATIC(name, type, field) \
LIST_GENERATE_INTERNAL(name, type, field, __attribute__((__unused__)) static)
#define LIST_GENERATE_INTERNAL(name, type, field, attrib) \
attrib void name##_LIST_APPEND(name* list, type* what)\
{\
    (what)->field.next = nullptr;\
	(what)->field.prev = nullptr;\
	if ((list)->tail)\
		(list)->tail->field.next = what;\
	if (!(list)->head)\
		(list)->head = (what);\
	(what)->field.prev = (list->tail);\
	(list)->tail = what;\
	(list)->nNodes++;\
}\
attrib void name##_LIST_PREPEND(name* list, type* what)\
{\
    (what)->field.next = nullptr;\
	(what)->field.prev = nullptr;\
	if ((list)->head)\
		(list)->head->field.prev = what;\
	if (!(list)->tail)\
		(list)->tail = (what);\
	(what)->field.next = (list->head);\
	(list)->head = what;\
	(list)->nNodes++;\
}\
attrib void name##_LIST_REMOVE(name* list, type* what)\
{\
    if ((list)->tail == what)\
		(list)->tail = (what->field).prev;\
	if ((list)->head == what)\
		(list)->head = (what->field).next;\
	if ((what->field).prev)\
		(what->field).prev->field.next = (what->field).next;\
	if ((what->field).next)\
		(what->field).next->field.prev = (what->field).prev;\
	(list)->nNodes--;\
	(what->field).next = nullptr;\
	(what->field).prev = nullptr;\
}\
attrib type* name##_LIST_GET_NEXT(name* list, type* what)\
{\
	OBOS_UNUSED(list);\
	return what->field.next;\
}\
attrib type* name##_LIST_GET_PREV(name* list, type* what)\
{\
	OBOS_UNUSED(list);\
	return what->field.prev;\
}

#define LIST_APPEND(name, list, x)\
name##_LIST_APPEND(list, x)
#define LIST_PREPEND(name, list, x)\
name##_LIST_PREPEND(list, x)
#define LIST_REMOVE(name, list, x)\
name##_LIST_REMOVE(list, x)
#define LIST_GET_NODE_COUNT(name, list)\
((list)->nNodes)
#define LIST_GET_NEXT(name, list, node)\
name##_LIST_GET_NEXT(list, node)
#define LIST_GET_PREV(name, list, node)\
name##_LIST_GET_PREV(list, node)
#define LIST_GET_HEAD(name, list)\
(list)->head
#define LIST_GET_TAIL(name, list)\
(list)->tail
#define LIST_IS_NODE_UNLINKED(name, list, node)\
(LIST_GET_HEAD(name, list) != node && LIST_GET_NEXT(name, list, node) == nullptr && LIST_GET_PREV(name, list, node) == nullptr)
