#include "Core/Core.h"
#include "Core/MemoryArena.h"
#include "Core/Platform.h"
#include "WAST/WAST.h"
#include "Runtime/Runtime.h"
#include "Runtime/Linker.h"
#include "Runtime/Intrinsics.h"

#include "CLI.h"

using namespace WebAssembly;
using namespace Runtime;

void showHelp()
{
	std::cerr << "Usage: wavm [switches] [programfile] [--] [arguments]" << std::endl;
	std::cerr << "  in.wast\t\tSpecify text program file (.wast)" << std::endl;
	std::cerr << "  in.wasm\tSpecify binary program file (.wasm)" << std::endl;
	std::cerr << "  -f|--function name\t\tSpecify function name to run in module rather than main" << std::endl;
	std::cerr << "  -c|--check\t\t\tExit after checking that the program is valid" << std::endl;
	std::cerr << "  --\t\t\t\tStop parsing arguments" << std::endl;
}

struct RootResolver : Resolver
{
	std::map<std::string,Resolver*> moduleNameToResolverMap;

	bool resolve(const char* moduleName,const char* exportName,ObjectType type,Object*& outObject) override
	{
		// Try to resolve an intrinsic first.
		if(IntrinsicResolver::singleton.resolve(moduleName,exportName,type,outObject)) { return true; }

		// Then look for a named module.
		auto namedResolverIt = moduleNameToResolverMap.find(moduleName);
		if(namedResolverIt != moduleNameToResolverMap.end())
		{
			return namedResolverIt->second->resolve(moduleName,exportName,type,outObject);
		}

		return false;
	}
};

int commandMain(int argc,char** argv)
{
	const char* sourceFile = 0;
	const char* binaryFile = 0;
	const char* functionName = 0;

	bool onlyCheck = false;
	auto args = argv;
	while(*++args)
	{
		if(!sourceFile && endsWith(*args, ".wast"))
		{
			sourceFile = *args;
		}
		else if(!binaryFile && endsWith(*args, ".wasm"))
		{
			binaryFile = *args;
		}
		else if(!strcmp(*args, "--function") || !strcmp(*args, "-f"))
		{
			if(!*++args) { showHelp(); return EXIT_FAILURE; }
			functionName = *args;
		}
		else if(!strcmp(*args, "--check") || !strcmp(*args, "-c"))
		{
			onlyCheck = true;
		}
		else if(!strcmp(*args, "--"))
		{
			++args;
			break;
		}
		else if(!strcmp(*args, "--help") || !strcmp(*args, "-h"))
		{
			showHelp();
			return EXIT_SUCCESS;
		}
		else { break; }
	}

	Module module;
	const char* main_arg0 = 0;
#ifdef __AFL_LOOP
        while (__AFL_LOOP(2000)) {
#endif
	if(sourceFile)
	{
		if(!loadTextModule(sourceFile,module)) { return EXIT_FAILURE; }
		main_arg0 = sourceFile;
	}
	else if(binaryFile)
	{
		if(!loadBinaryModule(binaryFile,module)) { return EXIT_FAILURE; }
		main_arg0 = binaryFile;
	}
	else
	{
		showHelp();
		return EXIT_FAILURE;
	}
#ifdef __AFL_LOOP
	delete module;
	module = nullptr;
	}
	return EXIT_SUCCESS;
#endif

	if(onlyCheck) { return EXIT_SUCCESS; }
		
	RootResolver rootResolver;

	Runtime::init();
	ModuleInstance* moduleInstance = linkAndInstantiateModule(module,rootResolver);
	if(!moduleInstance) { return EXIT_FAILURE; }

	FunctionInstance* functionInstance;
	if(!functionName)
	{
		functionInstance = asFunctionNullable(getInstanceExport(moduleInstance,"main"));
		if(!functionInstance) { functionInstance = asFunctionNullable(getInstanceExport(moduleInstance,"_main")); }
		if(!functionInstance)
		{
			std::cerr << "Module does not export main function" << std::endl;
			return EXIT_FAILURE;
		}
	}
	else
	{
		functionInstance = asFunctionNullable(getInstanceExport(moduleInstance,functionName));
		if(!functionInstance)
		{
			std::cerr << "Module does not export '" << functionName << "'" << std::endl;
			return EXIT_FAILURE;
		}
	}
	const FunctionType* functionType = getFunctionType(functionInstance);

	std::vector<Value> parameters;
	if(!functionName)
	{
		if(functionType->parameters.size() == 2)
		{
			Memory* defaultMemory = Runtime::getDefaultMemory(moduleInstance);
			if(!defaultMemory)
			{
				std::cerr << "Module does not declare a default memory object to put arguments in." << std::endl;
				return EXIT_FAILURE;
			}

			size_t argumentPageIndex = growMemory(defaultMemory,1);
			uint8* defaultMemoryBase = getMemoryBaseAddress(defaultMemory);
			uint8* argumentMemory = defaultMemoryBase + (argumentPageIndex << WebAssembly::numBytesPerPageLog2);

			uintptr main_argc_start = args-argv-1;
			auto main_argc = (uintptr)argc - main_argc_start;
				
			auto main_argv = (uint32*)argumentMemory;
			argumentMemory += (main_argc + 1) * sizeof(uint32);
			for (auto i = main_argc_start; i < (uint32)argc; ++i)
			{
				const char* string = argv[i];
				if(i == main_argc_start) { string = main_arg0; }
				auto stringSize = strlen(string)+1;
				auto stringMemory = argumentMemory;
				argumentMemory += stringSize;
				memcpy(stringMemory,string,stringSize);
				main_argv[i-main_argc_start] = (uint32)(stringMemory - getMemoryBaseAddress(defaultMemory));
			}
			main_argv[main_argc] = 0;
			parameters = {(uint32)main_argc, (uint32)((uint8*)main_argv - defaultMemoryBase) };
		}
		else if(functionType->parameters.size() > 0)
		{
			std::cerr << "WebAssembly function requires " << functionType->parameters.size() << " argument(s), but only 0 or 2 can be passed!" << std::endl;
			return EXIT_FAILURE;
		}
	}
	else
	{
		parameters.resize(functionType->parameters.size());
		uintptr main_argc_start = args-argv;
		auto end = (uintptr)std::min((uintptr)functionType->parameters.size(), (uintptr)(argc - main_argc_start));
		for(uint32 i = 0; i < end; ++i)
		{
			Value value;
			switch((TypeId)functionType->parameters[i])
			{
			case TypeId::unit: break;
			case TypeId::i32: value = (uint32)atoi(argv[main_argc_start+i]); break;
			case TypeId::i64: value = (uint64)atol(argv[main_argc_start+i]); break;
			case TypeId::f32:
			case TypeId::f64: value = atof(argv[main_argc_start+i]); break;
			default: throw;
			}
			parameters[i] = value;
		}
	}

	#if WAVM_TIMER_OUTPUT
	Core::Timer executionTime;
	#endif
	auto functionResult = invokeFunction(functionInstance,parameters);
	#if WAVM_TIMER_OUTPUT
	executionTime.stop();
	std::cout << "Execution time: " << executionTime.getMilliseconds() << "ms" << std::endl;
	#endif

	if(functionResult.type == TypeId::exception)
	{
		std::cerr << "WebAssembly module threw exception: " << describeExceptionCause(functionResult.exception->cause) << std::endl;
		for(auto calledFunction : functionResult.exception->callStack) { std::cerr << "  " << calledFunction << std::endl; }
		return EXIT_FAILURE;
	}

	if(!functionName)
	{
		if(functionResult.type != TypeId::i32) { return EXIT_SUCCESS; }
		return functionResult.i32;
	}

	std::cout << functionName << " returned: " << describeRuntimeValue(functionResult) << std::endl;
	return EXIT_SUCCESS;
}