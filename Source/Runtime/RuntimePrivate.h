#pragma once

#include "Inline/BasicTypes.h"
#include "Inline/HashMap.h"
#include "Runtime/Intrinsics.h"
#include "Runtime/Runtime.h"

#include <functional>
#include <atomic>

namespace Intrinsics { struct Module; }

namespace Runtime { enum class CallingConvention; }

namespace LLVMJIT
{
	using namespace Runtime;
	
	struct JITModuleBase
	{
		virtual ~JITModuleBase() {}
	};

	void instantiateModule(const IR::Module& module,Runtime::ModuleInstance* moduleInstance);
	bool describeInstructionPointer(Uptr ip,std::string& outDescription);
	
	typedef Runtime::ContextRuntimeData* (*InvokeFunctionPointer)(void*,Runtime::ContextRuntimeData*);

	// Generates an invoke thunk for a specific function type.
	InvokeFunctionPointer getInvokeThunk(
		IR::FunctionType functionType,
		Runtime::CallingConvention callingConvention);

	// Generates a thunk to call a native function from generated code.
	void* getIntrinsicThunk(
		void* nativeFunction,
		IR::FunctionType functionType,
		Runtime::CallingConvention callingConvention);
}

namespace Runtime
{
	using namespace IR;

	// A private root for all runtime objects that handles garbage collection.
	struct ObjectImpl : Object
	{
		std::atomic<Uptr> numRootReferences;

		ObjectImpl(ObjectKind inKind);

		// Called on all objects that are about to be deleted before any of them are deleted.
		virtual void finalize() {}
	};

	// An instance of a function: a function defined in an instantiated module, or an intrinsic function.
	struct FunctionInstance : ObjectImpl
	{
		ModuleInstance* moduleInstance;
		FunctionType type;
		void* nativeFunction;
		CallingConvention callingConvention;
		std::string debugName;

		FunctionInstance(
			ModuleInstance* inModuleInstance,
			FunctionType inType,
			void* inNativeFunction,
			CallingConvention inCallingConvention,
			std::string&& inDebugName)
		: ObjectImpl(ObjectKind::function)
		, moduleInstance(inModuleInstance)
		, type(inType)
		, nativeFunction(inNativeFunction)
		, callingConvention(inCallingConvention)
		, debugName(std::move(inDebugName))
		{}
	};

	// An instance of a WebAssembly Table.
	struct TableInstance : ObjectImpl
	{
		struct FunctionElement
		{
			FunctionType::Encoding typeEncoding;
			void* value;
		};

		Compartment* compartment;
		Uptr id;

		TableType type;

		FunctionElement* baseAddress;
		Uptr endOffset;

		// The Objects corresponding to the FunctionElements at baseAddress.
		Platform::Mutex elementsMutex;
		std::vector<Object*> elements;

		TableInstance(Compartment* inCompartment,const TableType& inType)
		: ObjectImpl(ObjectKind::table)
		, compartment(inCompartment)
		, id(UINTPTR_MAX)
		, type(inType)
		, baseAddress(nullptr)
		, endOffset(0)
		{}
		~TableInstance() override;
		virtual void finalize() override;
	};

	// An instance of a WebAssembly Memory.
	struct MemoryInstance : ObjectImpl
	{
		Compartment* compartment;
		Uptr id;

		MemoryType type;

		U8* baseAddress;
		std::atomic<Uptr> numPages;
		Uptr endOffset;

		MemoryInstance(Compartment* inCompartment,const MemoryType& inType)
		: ObjectImpl(ObjectKind::memory)
		, compartment(inCompartment)
		, id(UINTPTR_MAX)
		, type(inType)
		, baseAddress(nullptr)
		, numPages(0)
		, endOffset(0) {}
		~MemoryInstance() override;
		virtual void finalize() override;
	};

	// An instance of a WebAssembly global.
	struct GlobalInstance : ObjectImpl
	{
		Compartment* const compartment;
		Uptr id;

		const GlobalType type;
		const U32 mutableDataOffset;
		const UntaggedValue initialValue;

		GlobalInstance(Compartment* inCompartment,GlobalType inType,U32 inMutableDataOffset,UntaggedValue inInitialValue)
		: ObjectImpl(ObjectKind::global)
		, compartment(inCompartment)
		, id(UINTPTR_MAX)
		, type(inType)
		, mutableDataOffset(inMutableDataOffset)
		, initialValue(inInitialValue) {}
		virtual void finalize() override;
	};

	struct ExceptionData
	{
		ExceptionTypeInstance* typeInstance;
		U8 isUserException;
		UntaggedValue arguments[1];

		static Uptr calcNumBytes(Uptr numArguments)
		{
			return sizeof(ExceptionData) + (numArguments - 1) * sizeof(UntaggedValue);
		}
	};

	// An instance of a WebAssembly exception type.
	struct ExceptionTypeInstance : ObjectImpl
	{
		ExceptionType type;
		std::string debugName;

		ExceptionTypeInstance(ExceptionType inType, std::string&& inDebugName)
		: ObjectImpl(ObjectKind::exceptionTypeInstance)
		, type(inType)
		, debugName(std::move(inDebugName))
		{}
	};

	// An instance of a WebAssembly module.
	struct ModuleInstance : ObjectImpl
	{
		Compartment* compartment;

		HashMap<std::string, Object*> exportMap;

		std::vector<FunctionInstance*> functionDefs;

		std::vector<FunctionInstance*> functions;
		std::vector<TableInstance*> tables;
		std::vector<MemoryInstance*> memories;
		std::vector<GlobalInstance*> globals;
		std::vector<ExceptionTypeInstance*> exceptionTypeInstances;

		FunctionInstance* startFunction;
		MemoryInstance* defaultMemory;
		TableInstance* defaultTable;

		LLVMJIT::JITModuleBase* jitModule;

		std::string debugName;

		ModuleInstance(
			Compartment* inCompartment,
			std::vector<FunctionInstance*>&& inFunctionImports,
			std::vector<TableInstance*>&& inTableImports,
			std::vector<MemoryInstance*>&& inMemoryImports,
			std::vector<GlobalInstance*>&& inGlobalImports,
			std::vector<ExceptionTypeInstance*>&& inExceptionTypeInstanceImports,
			std::string&& inDebugName
			)
		: ObjectImpl(ObjectKind::module)
		, compartment(inCompartment)
		, functions(inFunctionImports)
		, tables(inTableImports)
		, memories(inMemoryImports)
		, globals(inGlobalImports)
		, exceptionTypeInstances(inExceptionTypeInstanceImports)
		, startFunction(nullptr)
		, defaultMemory(nullptr)
		, defaultTable(nullptr)
		, jitModule(nullptr)
		, debugName(std::move(inDebugName))
		{}

		~ModuleInstance() override;
	};

	struct Context : ObjectImpl
	{
		Compartment* compartment;
		Uptr id;
		struct ContextRuntimeData* runtimeData;

		Context(Compartment* inCompartment)
		: ObjectImpl(ObjectKind::context)
		, compartment(inCompartment)
		, id(UINTPTR_MAX)
		, runtimeData(nullptr)
		{}

		virtual void finalize() override;
	};

	#define compartmentReservedBytes (4ull * 1024 * 1024 * 1024)
	enum { maxThunkArgAndReturnBytes = 256 };
	enum { maxGlobalBytes = 4096 - maxThunkArgAndReturnBytes };
	enum { maxMemories = 255 };
	enum { maxTables = 256 };
	enum { compartmentRuntimeDataAlignmentLog2 = 32 };
	enum { contextRuntimeDataAlignment = 4096 };

	static_assert(
		sizeof(UntaggedValue) * IR::maxReturnValues <= maxThunkArgAndReturnBytes,
		"maxThunkArgAndReturnBytes must be large enough to hold IR::maxReturnValues * sizeof(UntaggedValue)"
		);

	struct Compartment : ObjectImpl
	{
		Platform::Mutex mutex;

		struct CompartmentRuntimeData* runtimeData;
		U8* unalignedRuntimeData;
		std::atomic<U32> numGlobalBytes;

		// These are weak references that aren't followed by the garbage collector.
		// If the referenced object is deleted, it will null the reference here.
		std::vector<GlobalInstance*> globals;
		std::vector<MemoryInstance*> memories;
		std::vector<TableInstance*> tables;
		std::vector<Context*> contexts;

		U8 initialContextGlobalData[maxGlobalBytes];

		ModuleInstance* wavmIntrinsics;

		Compartment();
		~Compartment() override;
	};

	struct ContextRuntimeData
	{
		U8 thunkArgAndReturnData[maxThunkArgAndReturnBytes];
		U8 globalData[maxGlobalBytes];
	};

	struct CompartmentRuntimeData
	{
		Compartment* compartment;
		U8* memories[maxMemories];
		TableInstance::FunctionElement* tables[maxTables];
		ContextRuntimeData contexts[1]; // Actually [maxContexts], but at least MSVC doesn't allow declaring arrays that large.
	};

	enum { maxContexts = 1024 * 1024 - offsetof(CompartmentRuntimeData,contexts) / sizeof(ContextRuntimeData) };

	static_assert(sizeof(ContextRuntimeData) == 4096,"");
	static_assert(offsetof(CompartmentRuntimeData,contexts) % 4096 == 0,"CompartmentRuntimeData::contexts isn't page-aligned");
	//static_assert(offsetof(CompartmentRuntimeData,contexts[maxContexts]) == 4ull * 1024 * 1024 * 1024,"CompartmentRuntimeData isn't the expected size");
    static_assert(offsetof(CompartmentRuntimeData, contexts) + maxContexts * sizeof(ContextRuntimeData) == 4ull * 1024 * 1024 * 1024, "CompartmentRuntimeData isn't the expected size");

	inline CompartmentRuntimeData* getCompartmentRuntimeData(ContextRuntimeData* contextRuntimeData)
	{
		return reinterpret_cast<CompartmentRuntimeData*>(reinterpret_cast<Uptr>(contextRuntimeData) & 0xffffffff00000000);
	}

	DECLARE_INTRINSIC_MODULE(wavmIntrinsics);

	// Initializes global state used by the WAVM intrinsics.
	Runtime::ModuleInstance* instantiateWAVMIntrinsics(Compartment* compartment);

	// Checks whether an address is owned by a table or memory.
	bool isAddressOwnedByTable(U8* address);
	bool isAddressOwnedByMemory(U8* address);
}
