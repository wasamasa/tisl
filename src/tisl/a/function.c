//
// TISL/src/tisl/c/function.c
// TISL Ver 4.x
//

#include <memory.h>
#define TISL_VM_STRUCT
#include "../../../include/tni.h"
#include "../object.h"
#include "../vm.h"
#include "../tisl.h"
#include "../gc.h"
#include "../writer.h"
#include "../translator.h"
#include "../built_in_object.h"

///////////////////////////////////////
// CELL_FUNCTION

#define FUNCTION_MACRO							0x00000001
#define FUNCTION_MACRO_							0xfffffffe
#define FUNCTION_HEAP							0x00000002
#define FUNCTION_HEAP_							0xfffffffd

#define FUNCTION_GET_MACRO(func)				((func)->ui&FUNCTION_MACRO)
#define FUNCTION_SET_MACRO(func)				((func)->ui|=FUNCTION_MACRO)
#define FUNCTION_RESET_MACRO(func)				((func)->ui&=FUNCTION_MACRO_)
#define FUNCTION_GET_HEAP(func)					((func)->ui&FUNCTION_HEAP)
#define FUNCTION_SET_HEAP(func)					((func)->ui|=FUNCTION_HEAP)
#define FUNCTION_RESET_HEAP(func)				((func)->ui&=FUNCTION_HEAP_)

#define FUNCTION_GET_SIZE(func)					(((func)+1)->ui)
#define FUNCTION_SET_SIZE(func, size)			(((func)+1)->ui=(size))
#define FUNCTION_GET_PLIST(func)				(((func)+2)->cell)
#define FUNCTION_SET_PLIST(func, list)			(((func)+2)->cell=(list))
#define FUNCTION_GET_CODE_SIZE(func)			(((func)+3)->i)
#define FUNCTION_SET_CODE_SIZE(func, size)		(((func)+3)->i=(size))
#define FUNCTION_GET_PACKAGE(func)				(((func)+4)->cell)
#define FUNCTION_SET_PACKAGE(func, package)		(((func)+4)->cell=(package))
#define FUNCTION_GET_MAX_STACK(func)			(((func)+5)->i)
#define FUNCTION_SET_MAX_STACK(func, max)		(((func)+5)->i=(max))
#define FUNCTION_GET_USE_OBJECT_LIST(func)		(((func)+6)->cell)
#define FUNCTION_SET_USE_OBJECT_LIST(func, p)	(((func)+6)->cell=(p))
#define FUNCTION_GET_BODY(func)					((tPOBJECT)((func)+7))
#define FUNCTION_SET_BODY(func, obj)			(*FUNCTION_GET_BODY(func)=*(obj))
/*!!!*/
#define FUNCTION_GET_CODE_HEAD(func)		((unsigned char*)((func)+9))

VM_RET function_create(tPVM vm, tPCELL plist, const tINT size, const tINT max, tPCELL package, tPCELL* cell)
{
	VM_RET ret;
	if (plist) {
		tOBJECT obj;
		cell_to_object(plist, &obj);
		if (vm_push(vm, &obj)) return VM_ERROR;
	}
	ret=function_create_(vm, plist, size, max, package, cell);
	if (plist) vm_pop(vm);
	return ret;
}

VM_RET function_create_(tPVM vm, tPCELL plist, const tINT size, const tINT max, tPCELL package, tPCELL* cell)
{
	tUINT s;
	s=allocate_cell(vm, sizeof(tCELL)*9+sizeof(unsigned char)*size, cell);
	if (!s) return signal_condition(vm, TISL_ERROR_STORAGE_EXHAUSTED);
	CELL_SET_TYPE(*cell, CELL_FUNCTION);
	FUNCTION_SET_SIZE(*cell, s);
	FUNCTION_SET_PLIST(*cell, plist);
	FUNCTION_SET_CODE_SIZE(*cell, size);
	FUNCTION_SET_PACKAGE(*cell, package);
	FUNCTION_SET_MAX_STACK(*cell, max);
	FUNCTION_SET_BODY(*cell, &unbound);
	FUNCTION_SET_USE_OBJECT_LIST(*cell, 0);
	memset(FUNCTION_GET_CODE_HEAD(*cell), 0, sizeof(unsigned char)*size);
	if (plist&&parameter_list_is_heap(plist)) {
		FUNCTION_SET_HEAP(*cell);
	} else {
		FUNCTION_RESET_HEAP(*cell);
	}
	return VM_OK;
}

tUINT function_get_size(tPCELL function)
{
	return FUNCTION_GET_SIZE(function);
}

VM_RET function_write(tPVM vm, tPCELL stream, tPOBJECT obj)
{
	tPCELL func=OBJECT_GET_CELL(obj);
	tOBJECT lambda_list;
	if (function_is_macro(func)) {
		if (write_string(vm, stream, "#i(<macro> ")) return VM_ERROR;
	} else {
		if (write_string(vm, stream, "#i(<function> ")) return VM_ERROR;
	}
	// lambda-list
	if (FUNCTION_GET_PLIST(func)) {
		cons_get_cdr(FUNCTION_GET_PLIST(func), &lambda_list);
	} else {
		OBJECT_SET_NIL(&lambda_list);
	}
	if (write_object(vm, stream, &lambda_list)) return VM_ERROR;
	// body
	if (write_string(vm, stream, " ")) return VM_ERROR;
	if (write_object(vm, stream, FUNCTION_GET_BODY(func))) return VM_ERROR;
	return write_string(vm, stream, ")");
}

unsigned char* function_get_code_head(tPCELL function)
{
	return FUNCTION_GET_CODE_HEAD(function);
}

VM_RET function_write_code(tPVM vm, tPCELL function, const tINT pc, unsigned char c)
{
	unsigned char* head=FUNCTION_GET_CODE_HEAD(function);
	tINT size=(tINT)FUNCTION_GET_CODE_SIZE(function);
	if ((pc<0)||(pc>=size)) return signal_condition(vm, TISL_ERROR_SYSTEM_ERROR);
	head[pc]=c;
	return VM_OK;
}

VM_RET function_write_code_uint(tPVM vm, tPCELL function, const tINT pc, unsigned int i)
{
	unsigned char* head=FUNCTION_GET_CODE_HEAD(function);
	tINT size=(tINT)FUNCTION_GET_CODE_SIZE(function);
	if ((pc<0)||(pc+4>=size)) return signal_condition(vm, TISL_ERROR_SYSTEM_ERROR);
	*((unsigned int*)(head+pc))=i;
	return VM_OK;
}

static VM_RET function_call__(tPVM vm, void* code);

VM_RET function_call(tPVM vm, tPCELL function, tPCELL environment, tPOBJECT value)
{
	tPCELL last_function, last_environment, last_package;
//	tPOBJECT last_invoke_point;
	VM_RET ret;

	// 制御スタックを監視すべきでは？/*!!!*/
	vm->function_call_n++;
	if (vm->function_call_n>VM_MAX_FUNCTION_CALL) { vm->function_call_n--; return signal_condition(vm, TISL_ERROR_STACK_OVERFLOW); }

	if (vm_check_tisl_state(vm)) { vm->function_call_n--; return VM_ERROR; }
	if (vm_check_stack_overflow(vm, FUNCTION_GET_MAX_STACK(function))) { vm->function_call_n--; return VM_ERROR; }

	last_function=vm->function;
	last_environment=vm->environment;
	last_package=vm->current_package;
//	last_invoke_point=vm_get_invoke_point(vm);
	vm->function=function;
	vm->environment=environment;
	vm->current_package=FUNCTION_GET_PACKAGE(function);
//	vm_set_invoke_point_sp(vm);
	if (last_environment) {
		tOBJECT tmp;
		OBJECT_SET_ENVIRONMENT(&tmp, last_environment);
		if (vm_push_temp(vm, &tmp)) { vm->function_call_n--; return VM_ERROR; }
	}
	ret=function_call__(vm, function_get_code_head(function));
	if (ret) {
		OBJECT_SET_UNBOUND(value);
	} else {
		vm_top(vm, value);
		vm_pop(vm);
	}
	vm->function=last_function;
	vm->environment=last_environment;
	vm->current_package=last_package;
//	vm_set_invoke_point(vm, last_invoke_point);
	if (last_environment) vm_pop_temp(vm);
	vm->function_call_n--;
	return ret;
}

VM_RET function_call_(tPVM vm, tPCELL function, tPOBJECT value)
{
	// 関数内関数の呼び出しVMの関数情報は変更させないバージョン
	if (function_call__(vm, function_get_code_head(function))) {
		OBJECT_SET_UNBOUND(value);
		return VM_ERROR;
	} else {
		vm_top(vm, value);
		vm_pop(vm);
		return VM_OK;
	}
}

static VM_RET function_call__(tPVM vm, void* code)
{
	_asm{
		mov		eax, vm
		call	code
	};
	return vm_last_condition_is_ok(vm) ? VM_OK : VM_ERROR;
}

tPCELL function_get_package(tPCELL function)
{
	return FUNCTION_GET_PACKAGE(function);
}

tINT function_get_parameter_number(tPCELL function)
{
	return parameter_list_get_number(FUNCTION_GET_PLIST(function));
}

tINT function_get_code_size(tPCELL function)
{
	return FUNCTION_GET_CODE_SIZE(function);
}

tBOOL function_is_rest(tPCELL function)
{
	return parameter_list_is_rest(FUNCTION_GET_PLIST(function));
}

tBOOL function_is_heap(tPCELL function)
{
	return FUNCTION_GET_HEAP(function) ? tTRUE : tFALSE;
}

// 関数の使用するオブジェクトの登録 主に関数になるはず
VM_RET function_add_use_object(tPVM vm, tPCELL function, tPOBJECT obj)
{
	tPCELL p;
	tOBJECT rest;
	if (FUNCTION_GET_USE_OBJECT_LIST(function)) {
		OBJECT_SET_CONS(&rest, FUNCTION_GET_USE_OBJECT_LIST(function));
	} else {
		OBJECT_SET_NIL(&rest);
	}
	if (cons_create(vm, &p, obj, &rest)) return VM_ERROR;
	FUNCTION_SET_USE_OBJECT_LIST(function, p);
	return VM_OK;
}

tBOOL function_is_macro(tPCELL function)
{
	return FUNCTION_GET_MACRO(function) ? tTRUE : tFALSE;
}

void function_set_macro(tPCELL function)
{
	FUNCTION_SET_MACRO(function);
}

void function_set_body(tPCELL function, tPOBJECT body)
{
	FUNCTION_SET_BODY(function, body);
}

void function_get_body(tPCELL function, tPOBJECT body)
{
	*body=*FUNCTION_GET_BODY(function);
}

VM_RET function_mark(tPVM vm, tPCELL cell)
{
	if (gc_push(vm, FUNCTION_GET_BODY(cell))) return VM_ERROR;
	if (cell_mark(vm, FUNCTION_GET_USE_OBJECT_LIST(cell))) return VM_ERROR;
	if (cell_mark(vm, FUNCTION_GET_PLIST(cell))) return VM_ERROR;
	return VM_OK;
}

