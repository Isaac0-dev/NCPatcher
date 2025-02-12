#include "main.hpp"

#include <vector>
#include <filesystem>
#include <sstream>

#include "types.hpp"
#include "process.hpp"
#include "log.hpp"
#include "except.hpp"
#include "config/buildconfig.hpp"
#include "config/buildtarget.hpp"
#include "config/rebuildconfig.hpp"
#include "ndsbin/headerbin.hpp"
#include "ndsbin/armbin.hpp"
#include "build/sourcefilejob.hpp"
#include "build/objmaker.hpp"
#include "patch/patchmaker.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX 1
#endif
#include <windows.h>
#elif __linux__
#include <unistd.h>
#elif __APPLE__
#include <mach-o/dyld.h>
#include <limits.h>
#else
#error Unsupported operating system
#endif

namespace fs = std::filesystem;

namespace Main {

static std::filesystem::path s_appPath;
static std::filesystem::path s_workPath;
static std::filesystem::path s_romPath;
static const char* s_errorContext = nullptr;
static bool s_verbose = false;

const std::filesystem::path& getAppPath() { return s_appPath; }
const std::filesystem::path& getWorkPath() { return s_workPath; }
const std::filesystem::path& getRomPath() { return s_romPath; }
void setErrorContext(const char* errorContext) { s_errorContext = errorContext; }
bool getVerbose() { return s_verbose; }

}

static void runCommandList(const std::vector<std::string>& buildCmds, const char* msg, const char* errorCtx);

static void ncpMain()
{
	Log::out << ANSI_bWHITE " ----- Nitro Code Patcher -----" ANSI_RESET << std::endl;

	BuildConfig::load();
	RebuildConfig::load();

	const std::string& toolchain = BuildConfig::getToolchain();
	std::string gccPath = toolchain + "gcc";
	if (!Process::exists(gccPath.c_str()))
	{
		std::ostringstream oss;
		oss << "The building toolchain " << OSTR(toolchain) << " was not found." << OREASONNL;
		oss << "Make sure that it is correctly specified in the " << OSTR("ncpatcher.json") << " file and that it is present on your system.";
		throw ncp::exception(oss.str());
	}

	const fs::path& workDir = Main::getWorkPath();
	Main::s_romPath = fs::absolute(BuildConfig::getFilesystemDir());

	HeaderBin header;
	header.load(Main::s_romPath / "header.bin");

	bool forceRebuild = false;

	auto doWorkOnTarget = [&](bool isArm9){
		fs::current_path(Main::getWorkPath());

		Log::info(isArm9 ?
			"Loading ARM9 target configuration..." :
			"Loading ARM7 target configuration...");

		const fs::path& targetPath = fs::absolute(isArm9 ? BuildConfig::getArm9Target() : BuildConfig::getArm7Target());

		Main::setErrorContext(isArm9 ?
			"Could not load the ARM9 target configuration." :
			"Could not load the ARM7 target configuration.");
		BuildTarget buildTarget;
		buildTarget.load(targetPath, isArm9);
		Main::setErrorContext(nullptr);

		std::time_t lastTargetWriteTimeNew = buildTarget.getLastWriteTime();
		std::time_t lastTargetWriteTimeOld = isArm9 ?
			RebuildConfig::getArm9TargetWriteTime() :
			RebuildConfig::getArm7TargetWriteTime();
		buildTarget.setForceRebuild(forceRebuild || (lastTargetWriteTimeNew > lastTargetWriteTimeOld));

		Main::setErrorContext(isArm9 ?
			"Could not compile the ARM9 target." :
			"Could not compile the ARM7 target.");

		fs::path targetDir = targetPath.parent_path();
		fs::path buildPath = fs::absolute(isArm9 ? BuildConfig::getArm9BuildDir() : BuildConfig::getArm7BuildDir());

		std::vector<std::unique_ptr<SourceFileJob>> srcFileJobs;

		ObjMaker objMaker;
		objMaker.makeTarget(buildTarget, targetDir, buildPath, srcFileJobs);

		PatchMaker patchMaker;
		patchMaker.makeTarget(buildTarget, targetDir, buildPath, header, srcFileJobs);

		isArm9 ?
			RebuildConfig::setArm9TargetWriteTime(lastTargetWriteTimeNew) :
			RebuildConfig::setArm7TargetWriteTime(lastTargetWriteTimeNew);

		Main::setErrorContext(nullptr);
	};

	runCommandList(BuildConfig::getPreBuildCmds(), "Running pre-build commands...", "Not all pre-build commands succeeded.");

	const std::vector<std::string>& preBuildCmds = BuildConfig::getPreBuildCmds();

	if (BuildConfig::getLastWriteTime() > RebuildConfig::getBuildConfigWriteTime())
		forceRebuild = true;

	if (BuildConfig::getBuildArm7())
		doWorkOnTarget(false);

	if (BuildConfig::getBuildArm9())
		doWorkOnTarget(true);

	RebuildConfig::setBuildConfigWriteTime(BuildConfig::getLastWriteTime());
	RebuildConfig::save();

	runCommandList(BuildConfig::getPostBuildCmds(), "Running post-build commands...", "Not all post-build commands succeeded.");

	Log::info("All tasks finished.");
}

static void runCommandList(const std::vector<std::string>& buildCmds, const char* msg, const char* errorCtx)
{
	if (buildCmds.empty())
		return;

	Log::info(msg);

	Main::setErrorContext(errorCtx);

	int i = 1;
	for (const std::string& buildCmd : buildCmds)
	{
		std::ostringstream oss;
		oss << ANSI_bWHITE "[#" << i << "] " ANSI_bYELLOW << buildCmd << ANSI_RESET;
		Log::info(oss.str());

		fs::current_path(Main::getWorkPath());

		int retcode = Process::start(buildCmd.c_str(), &std::cout);
		if (retcode != 0)
			throw ncp::exception("Process returned: " + std::to_string(retcode));
		
		i++;
	}

	Main::setErrorContext(nullptr);
}

static std::filesystem::path fetchAppPath()
{
	// Copied from arclight.filesystem

#ifdef _WIN32

	u32 length = 0x200;
	std::vector<wchar_t> filename;

	try
	{
		filename.resize(length);
		while (GetModuleFileNameW(nullptr, filename.data(), length) == length)
		{
			if (length < 0x8000)
			{
				length *= 2;
				filename.resize(length);
			}
			else
			{
				/*
					Ideally, this cannot happen because the windows path limit is specified to be 0x7FFF (excl. null terminator byte)
					If this changes in future windows versions, long path names could fail since it would require to allocate fairly large buffers
					This is why we stop here with an error.
				*/
				throw std::runtime_error("Could not query application directory path: Path too long");
			}
		}

		std::wstring str(filename.data());
		return std::filesystem::path(str).parent_path();
	}
	catch (std::exception& e)
	{
		throw std::runtime_error(std::string("Could not query application directory path: ") + e.what());
	}

#elif __linux__

	constexpr const char* symlinkName = "/proc/self/exe";
	SizeT length = 0x200;

	std::vector<char> filename(length);

	try
	{
		while(true)
		{
			ssize_t readLength = readlink(symlinkName, filename.data(), filename.size());

			if (readLength == length)
			{
				//If length exceeds 0x10000 bytes, cancel
				if(length >= 0x10000)
					throw std::runtime_error("Could not query application directory path: Path name exceeds 0x10000 bytes");

				//Double buffer and retry
				length *= 2;
				filename.resize(length);
			}
			else if (readLength == -1)
			{
				//Error occured while reading the symlink
				throw std::runtime_error("Could not query application directory path: Cannot read symbolic link");
			}
			else
			{
				//Read was successful, return filename
				std::string str(filename.data(), readLength);
				return std::filesystem::path(str).parent_path();
			}
		}
	}
	catch (std::exception& e)
	{
		throw std::runtime_error(std::string("Could not query application directory path: ") + e.what());
	}

#elif __APPLE__

	char buf[PATH_MAX];
	uint32_t bufsize = PATH_MAX;
	if (_NSGetExecutablePath(buf, &bufsize) != 0)
		throw std::runtime_error("Could not query application directory path.");
	return std::filesystem::path(buf).parent_path();

#endif
}

int main(int argc, char* argv[])
{
	Log::init();

	try {
		Main::s_appPath = fetchAppPath();
	} catch (std::exception& ex) {
		Log::error(ex.what());
		return 1;
	}

	try {
		Main::s_workPath = fs::current_path();
	} catch (std::exception& ex) {
		Log::error("Could not query the application work directory path.");
		return 1;
	}

	try {
		Log::openLogFile(Main::s_appPath / "log.txt");
	} catch (std::exception& ex) {
		Log::error("Could not open the log file for writing.");
		return 1;
	}

	if (argc > 1)
		Main::s_verbose = ((strcmp(argv[1], "--verbose") == 0) || (strcmp(argv[1], "-v") == 0));

	try
	{
		ncpMain();
	}
	catch (std::exception& e)
	{
		Log::out << OERROR;
		if (Main::s_errorContext)
			Log::out << Main::s_errorContext << "\n" << OREASON;
		Log::out << e.what() << std::endl;
		return 1;
	}

	return 0;
}
