#include <pconsole.h>
#include <windows.h>
#include <assert.h>
#include <string.h>
#include <stdint.h>
#include <string>
#include <vector>
#include <sstream>
#include "../Shared/DebugClient.h"
#include "../Shared/AgentMsg.h"
#include "../Shared/Buffer.h"

// TODO: Error handling, handle out-of-memory.

#define AGENT_EXE L"pconsole-agent.exe"

static volatile LONG consoleCounter;

struct pconsole_s {
    pconsole_s();
    HANDLE controlPipe;
    HANDLE dataPipe;
};

pconsole_s::pconsole_s() : controlPipe(NULL), dataPipe(NULL)
{
}

static HMODULE getCurrentModule()
{
    HMODULE module;
    if (!GetModuleHandleEx(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                (LPCTSTR)getCurrentModule,
                &module)) {
        assert(false);
    }
    return module;
}

static std::wstring getModuleFileName(HMODULE module)
{
    const int bufsize = 4096;
    wchar_t path[bufsize];
    int size = GetModuleFileName(module, path, bufsize);
    assert(size != 0 && size != bufsize);
    return std::wstring(path);
}

static std::wstring dirname(const std::wstring &path)
{
    std::wstring::size_type pos = path.find_last_of(L"\\/");
    if (pos == std::wstring::npos)
        return L"";
    else
        return path.substr(0, pos);
}

static bool pathExists(const std::wstring &path)
{
    return GetFileAttributes(path.c_str()) != 0xFFFFFFFF;
}

static std::wstring findAgentProgram()
{
    std::wstring progDir = dirname(getModuleFileName(getCurrentModule()));
    std::wstring ret = progDir + L"\\"AGENT_EXE;
    assert(pathExists(ret));
    return ret;
}

// Call ConnectNamedPipe and block, even for an overlapped pipe.  If the
// pipe is overlapped, create a temporary event for use connecting.
static bool connectNamedPipe(HANDLE handle, bool overlapped)
{
    OVERLAPPED over, *pover = NULL;
    if (overlapped) {
        pover = &over;
        memset(&over, 0, sizeof(over));
        over.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
        assert(over.hEvent != NULL);
    }
    bool success = ConnectNamedPipe(handle, pover);
    if (overlapped && !success && GetLastError() == ERROR_IO_PENDING) {
        DWORD actual;
        success = GetOverlappedResult(handle, pover, &actual, TRUE);
    }
    if (!success && GetLastError() == ERROR_PIPE_CONNECTED)
        success = TRUE;
    if (overlapped)
        CloseHandle(over.hEvent);
    return success;
}

static void writePacket(pconsole_t *pc, const WriteBuffer &packet)
{
    std::string payload = packet.str();
    int32_t payloadSize = payload.size();
    DWORD actual;
    BOOL success = WriteFile(pc->controlPipe, &payloadSize, sizeof(int32_t), &actual, NULL);
    assert(success && actual == sizeof(int32_t));
    success = WriteFile(pc->controlPipe, payload.c_str(), payloadSize, &actual, NULL);
    assert(success && (int32_t)actual == payloadSize);
}

static int32_t readInt32(pconsole_t *pc)
{
    int32_t result;
    DWORD actual;
    BOOL success = ReadFile(pc->controlPipe, &result, sizeof(int32_t), &actual, NULL);
    assert(success && actual == sizeof(int32_t));
    return result;
}

static HANDLE createNamedPipe(const std::wstring &name, bool overlapped)
{
    return CreateNamedPipe(name.c_str(),
                           /*dwOpenMode=*/
                           PIPE_ACCESS_DUPLEX |
                           FILE_FLAG_FIRST_PIPE_INSTANCE |
                           (overlapped ? FILE_FLAG_OVERLAPPED : 0),
                           /*dwPipeMode=*/0,
                           /*nMaxInstances=*/1,
                           /*nOutBufferSize=*/0,
                           /*nInBufferSize=*/0,
                           /*nDefaultTimeOut=*/3000,
                           NULL);
}

struct BackgroundDesktop {
    HWINSTA originalStation;
    HWINSTA station;
    HDESK desktop;
    std::wstring desktopName;
};

// Get a non-interactive window station for the agent.
// TODO: review security w.r.t. windowstation and desktop.
static BackgroundDesktop setupBackgroundDesktop()
{
    BackgroundDesktop ret;
    ret.originalStation = GetProcessWindowStation();
    ret.station = CreateWindowStation(NULL, 0, WINSTA_ALL_ACCESS, NULL);
    bool success = SetProcessWindowStation(ret.station);
    assert(success);
    ret.desktop = CreateDesktop(L"Default", NULL, NULL, 0, GENERIC_ALL, NULL);
    assert(ret.originalStation != NULL);
    assert(ret.station != NULL);
    assert(ret.desktop != NULL);
    wchar_t stationNameWStr[256];
    success = GetUserObjectInformation(ret.station, UOI_NAME,
                                       stationNameWStr, sizeof(stationNameWStr),
                                       NULL);
    assert(success);
    ret.desktopName = std::wstring(stationNameWStr) + L"\\Default";
    return ret;
}

static void restoreOriginalDesktop(const BackgroundDesktop &desktop)
{
    SetProcessWindowStation(desktop.originalStation);
    CloseDesktop(desktop.desktop);
    CloseWindowStation(desktop.station);
}

static void startAgentProcess(const BackgroundDesktop &desktop,
                              std::wstring &controlPipeName,
                              std::wstring &dataPipeName, 
                              int cols, int rows)
{
    bool success;

    std::wstring agentProgram = findAgentProgram();
    std::wstringstream agentCmdLineStream;
    agentCmdLineStream << L"\"" << agentProgram << L"\" "
                       << controlPipeName << " " << dataPipeName << " "
                       << cols << " " << rows;
    std::wstring agentCmdLine = agentCmdLineStream.str();

    // Start the agent.
    STARTUPINFO sui;
    memset(&sui, 0, sizeof(sui));
    sui.cb = sizeof(sui);
    sui.lpDesktop = (LPWSTR)desktop.desktopName.c_str();
    PROCESS_INFORMATION pi;
    memset(&pi, 0, sizeof(pi));
    std::vector<wchar_t> cmdline(agentCmdLine.size() + 1);
    agentCmdLine.copy(&cmdline[0], agentCmdLine.size());
    cmdline[agentCmdLine.size()] = L'\0';
    success = CreateProcess(agentProgram.c_str(),
                            &cmdline[0],
                            NULL, NULL,
                            /*bInheritHandles=*/FALSE,
                            /*dwCreationFlags=*/CREATE_NEW_CONSOLE,
                            NULL, NULL,
                            &sui, &pi);
    if (!success) {
        assert(false);
    }

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
}

PCONSOLE_API pconsole_t *pconsole_open(int cols, int rows)
{
    pconsole_t *pc = new pconsole_t;

    // Start pipes.
    std::wstringstream pipeName;
    pipeName << L"\\\\.\\pipe\\pconsole-" << GetCurrentProcessId()
             << L"-" << InterlockedIncrement(&consoleCounter);
    std::wstring controlPipeName = pipeName.str() + L"-control";
    std::wstring dataPipeName = pipeName.str() + L"-data";
    pc->controlPipe = createNamedPipe(controlPipeName, false);
    if (pc->controlPipe == INVALID_HANDLE_VALUE) {
        delete pc;
        return NULL;
    }
    pc->dataPipe = createNamedPipe(dataPipeName, true);
    if (pc->dataPipe == INVALID_HANDLE_VALUE) {
        delete pc;
        return NULL;
    }

    // Setup a background desktop for the agent.
    BackgroundDesktop desktop = setupBackgroundDesktop();

    // Start the agent.
    startAgentProcess(desktop, controlPipeName, dataPipeName, cols, rows);

    // TODO: Frequently, I see the CreateProcess call return successfully,
    // but the agent immediately dies.  The following pipe connect calls then
    // hang.  These calls should probably timeout.  Maybe this code could also
    // poll the agent process handle?

    // Connect the pipes.
    bool success;
    success = connectNamedPipe(pc->controlPipe, false);
    if (!success) {
        delete pc;
        return NULL;
    }
    success = connectNamedPipe(pc->dataPipe, true);
    if (!success) {
        delete pc;
        return NULL;
    }

    // Close handles to the background desktop and restore the original window
    // station.  This must wait until we know the agent is running -- if we
    // close these handles too soon, then the desktop and windowstation will be
    // destroyed before the agent can connect with them.
    restoreOriginalDesktop(desktop);

    // The default security descriptor for a named pipe allows anyone to connect
    // to the pipe to read, but not to write.  Only the "creator owner" and
    // various system accounts can write to the pipe.  By sending and receiving
    // a dummy message on the control pipe, we should confirm that something
    // trusted (i.e. the agent we just started) successfully connected and wrote
    // to one of our pipes.
    WriteBuffer packet;
    packet.putInt(AgentMsg::Ping);
    writePacket(pc, packet);
    if (readInt32(pc) != 0) {
        delete pc;
        return NULL;
    }

    // TODO: On Windows Vista and forward, we could call
    // GetNamedPipeClientProcessId and verify that the PID is correct.  We could
    // also pass the PIPE_REJECT_REMOTE_CLIENTS flag on newer OS's.
    // TODO: I suppose this code is still subject to a denial-of-service attack
    // from untrusted accounts making read-only connections to the pipe.  It
    // should probably provide a SECURITY_DESCRIPTOR for the pipe, but the last
    // time I tried that (using SDDL), I couldn't get it to work (access denied
    // errors).

    // Aside: An obvious way to setup these handles is to open both ends of the
    // pipe in the parent process and let the child inherit its handles.
    // Unfortunately, the Windows API makes inheriting handles problematic.
    // MSDN says that handles have to be marked inheritable, and once they are,
    // they are inherited by any call to CreateProcess with
    // bInheritHandles==TRUE.  To avoid accidental inheritance, the library's
    // clients would be obligated not to create new processes while a thread
    // was calling pconsole_open.  Moreover, to inherit handles, MSDN seems
    // to say that bInheritHandles must be TRUE[*], but I don't want to use a
    // TRUE bInheritHandles, because I want to avoid leaking handles into the
    // agent process, especially if the library someday allows creating the
    // agent process under a different user account.
    //
    // [*] The way that bInheritHandles and STARTF_USESTDHANDLES work together
    // is unclear in the documentation.  On one hand, for STARTF_USESTDHANDLES,
    // it says that bInheritHandles must be TRUE.  On Vista and up, isn't
    // PROC_THREAD_ATTRIBUTE_HANDLE_LIST an acceptable alternative to
    // bInheritHandles?  On the other hand, KB315939 contradicts the
    // STARTF_USESTDHANDLES documentation by saying, "Your pipe handles will
    // still be duplicated because Windows will always duplicate the STD
    // handles, even when bInheritHandles is set to FALSE."  IIRC, my testing
    // showed that the KB article was correct.

    return pc;
}

// TODO: We also need to control what desktop the child process is started with.
// I think the right default is for this pconsole.dll function to query the
// current desktop and send that to the agent.
PCONSOLE_API int pconsole_start_process(pconsole_t *pc,
					const wchar_t *appname,
					const wchar_t *cmdline,
					const wchar_t *cwd,
					const wchar_t *env)
{
    WriteBuffer packet;
    packet.putInt(AgentMsg::StartProcess);
    packet.putWString(appname ? appname : L"");
    packet.putWString(cmdline ? cmdline : L"");
    packet.putWString(cwd ? cwd : L"");
    std::wstring envStr;
    if (env != NULL) {
        const wchar_t *p = env;
        while (*p != L'\0') {
            p += wcslen(p) + 1;
        }
        p++;
        envStr.assign(env, p);
    }
    packet.putWString(envStr);
    writePacket(pc, packet);
    return readInt32(pc);
}

PCONSOLE_API int pconsole_get_exit_code(pconsole_t *pc)
{
    WriteBuffer packet;
    packet.putInt(AgentMsg::GetExitCode);
    writePacket(pc, packet);
    return readInt32(pc);
}

PCONSOLE_API HANDLE pconsole_get_data_pipe(pconsole_t *pc)
{
    return pc->dataPipe;
}

PCONSOLE_API int pconsole_set_size(pconsole_t *pc, int cols, int rows)
{
    WriteBuffer packet;
    packet.putInt(AgentMsg::SetSize);
    packet.putInt(cols);
    packet.putInt(rows);
    writePacket(pc, packet);
    return readInt32(pc);
}

PCONSOLE_API void pconsole_close(pconsole_t *pc)
{
    CloseHandle(pc->controlPipe);
    CloseHandle(pc->dataPipe);
    delete pc;
}