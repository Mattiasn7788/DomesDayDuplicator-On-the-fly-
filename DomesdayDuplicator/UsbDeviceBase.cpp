#include "UsbDeviceBase.h"
#include "DddFrontEndGain.h"
#ifdef _WIN32
#include <memoryapi.h>
#include <io.h>
#include <fcntl.h>

static std::wstring utf8ToWide(const std::string& text)
{
    if (text.empty())
        return std::wstring();
    const int required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (required <= 0)
        return std::wstring(text.begin(), text.end());
    std::wstring wide(static_cast<size_t>(required), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), wide.data(), required);
    return wide;
}

// Starts flac.exe without a shell or visible console window. The application writes
// signed-16 samples to the returned FILE* while flac writes the target file itself, so
// it can seek back and finalise STREAMINFO. Stderr is kept for diagnostics. Avoiding
// cmd.exe also keeps file names and metadata out of a shell command line.
static FILE* openFlacPipeNoWindow(const std::wstring& executable,
    const std::wstring& arguments, HANDLE& outProcess, HANDLE& outErrorReadPipe)
{
    outProcess = INVALID_HANDLE_VALUE;
    outErrorReadPipe = INVALID_HANDLE_VALUE;

    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    // Stdin pipe: our process writes to hWrite, child reads from hRead
    HANDLE hStdinRead, hStdinWrite;
    if (!CreatePipe(&hStdinRead, &hStdinWrite, &sa, 0))
        return nullptr;
    if (!SetHandleInformation(hStdinWrite, HANDLE_FLAG_INHERIT, 0))
    {
        CloseHandle(hStdinRead);
        CloseHandle(hStdinWrite);
        return nullptr;
    }

    // flac writes the actual file itself; its console output and diagnostics can share
    // this pipe without touching capture data.
    HANDLE hStderrRead, hStderrWrite;
    if (!CreatePipe(&hStderrRead, &hStderrWrite, &sa, 0))
    {
        CloseHandle(hStdinRead);
        CloseHandle(hStdinWrite);
        return nullptr;
    }
    if (!SetHandleInformation(hStderrRead, HANDLE_FLAG_INHERIT, 0))
    {
        CloseHandle(hStdinRead);
        CloseHandle(hStdinWrite);
        CloseHandle(hStderrRead);
        CloseHandle(hStderrWrite);
        return nullptr;
    }

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdInput  = hStdinRead;
    si.hStdOutput = hStderrWrite;
    si.hStdError  = hStderrWrite;

    PROCESS_INFORMATION pi = {};
    std::wstring commandLine = L"\"" + executable + L"\" " + arguments;
    std::vector<wchar_t> mutableCommandLine(commandLine.begin(), commandLine.end());
    mutableCommandLine.push_back(L'\0');
    BOOL ok = CreateProcessW(executable.c_str(), mutableCommandLine.data(),
                             NULL, NULL, TRUE, CREATE_NO_WINDOW,
                             NULL, NULL, &si, &pi);

    // Close handles we passed to the child — child has its own copies
    CloseHandle(hStdinRead);
    CloseHandle(hStderrWrite);

    if (!ok)
    {
        CloseHandle(hStdinWrite);
        CloseHandle(hStderrRead);
        return nullptr;
    }

    CloseHandle(pi.hThread);
    outProcess = pi.hProcess;
    outErrorReadPipe = hStderrRead;

    int fd = _open_osfhandle(reinterpret_cast<intptr_t>(hStdinWrite), _O_WRONLY | _O_BINARY);
    if (fd == -1)
    {
        CloseHandle(hStdinWrite);
        CloseHandle(hStderrRead);
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 5000);
        CloseHandle(pi.hProcess);
        outProcess = INVALID_HANDLE_VALUE;
        outErrorReadPipe = INVALID_HANDLE_VALUE;
        return nullptr;
    }
    FILE* pipeFile = _fdopen(fd, "wb");
    if (pipeFile == nullptr)
    {
        _close(fd); // also closes hStdinWrite, now owned by the CRT descriptor
        CloseHandle(hStderrRead);
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 5000);
        CloseHandle(pi.hProcess);
        outProcess = INVALID_HANDLE_VALUE;
        outErrorReadPipe = INVALID_HANDLE_VALUE;
        return nullptr;
    }
    return pipeFile;
}
#else
#include <sched.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <pthread.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#endif
#include <iostream>
#include <algorithm>
#include <thread>
#include <functional>
#include <cassert>
#include <cerrno>
#include <stdexcept>
#ifdef __APPLE__
#include <fcntl.h>
#include <unistd.h>
#endif

#ifndef _WIN32
static bool waitForFlacProcess(pid_t processId, int timeoutInMilliseconds, int& status)
{
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(timeoutInMilliseconds);
    while (true)
    {
        const pid_t waitResult = waitpid(processId, &status, WNOHANG);
        if (waitResult == processId)
            return true;
        if (waitResult < 0 && errno != EINTR)
            return false;
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

static void signalFlacProcessGroup(pid_t processId, int signalNumber)
{
    // The child creates its own process group before exec. Signal both the group
    // and the leader so a very early setpgid race cannot leave either the shell
    // or encoder behind.
    ::kill(-processId, signalNumber);
    ::kill(processId, signalNumber);
}

static bool terminateFlacProcess(pid_t processId, int& status)
{
    signalFlacProcessGroup(processId, SIGTERM);
    if (waitForFlacProcess(processId, 5000, status))
        return true;
    signalFlacProcessGroup(processId, SIGKILL);
    return waitForFlacProcess(processId, 5000, status);
}

static FILE* openFlacPipeProcess(const std::string& command, int& outProcessId)
{
    outProcessId = -1;
    int pipeHandles[2] = {-1, -1};
    if (pipe(pipeHandles) != 0)
        return nullptr;

    const pid_t processId = fork();
    if (processId < 0)
    {
        close(pipeHandles[0]);
        close(pipeHandles[1]);
        return nullptr;
    }

    if (processId == 0)
    {
        setpgid(0, 0);
        if (dup2(pipeHandles[0], STDIN_FILENO) < 0)
            _exit(127);
        close(pipeHandles[0]);
        close(pipeHandles[1]);
        execl("/bin/sh", "sh", "-c", command.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }

    // The child also calls setpgid; this parent-side call closes the small race
    // before it reaches exec. EACCES merely means the child won that race.
    setpgid(processId, processId);
    close(pipeHandles[0]);
    FILE* pipeFile = fdopen(pipeHandles[1], "wb");
    if (pipeFile == nullptr)
    {
        close(pipeHandles[1]);
        int status = 0;
        terminateFlacProcess(processId, status);
        return nullptr;
    }

    // ProcessingThread writes whole conversion buffers synchronously; disabling
    // stdio buffering ensures fclose never has a hidden backlog to flush.
    setvbuf(pipeFile, nullptr, _IONBF, 0);
    outProcessId = static_cast<int>(processId);
    return pipeFile;
}
#endif

//----------------------------------------------------------------------------------------------------------------------
// Constructors
//----------------------------------------------------------------------------------------------------------------------
UsbDeviceBase::UsbDeviceBase(const ILogger& log)
:log(log)
{ }

//----------------------------------------------------------------------------------------------------------------------
UsbDeviceBase::~UsbDeviceBase()
{ }

//----------------------------------------------------------------------------------------------------------------------
bool UsbDeviceBase::Initialize(uint16_t vendorId, uint16_t productId)
{
    targetDeviceVendorId = vendorId;
    targetDeviceProductId = productId;

    // Attempt to retrieve the initial process working set allocation
#ifdef _WIN32
    BOOL getProcessWorkingSetSizeReturn = GetProcessWorkingSetSize(GetCurrentProcess(), &originalProcessMinimumWorkingSetSizeInBytes, &originalProcessMaximumWorkingSetSizeInBytes);
    if (getProcessWorkingSetSizeReturn == 0)
    {
        DWORD lastError = GetLastError();
        Log().Error("GetProcessWorkingSetSize failed with error code {0}", lastError);
        return false;
    }
#endif
    return true;
}

//----------------------------------------------------------------------------------------------------------------------
// Log methods
//----------------------------------------------------------------------------------------------------------------------
const ILogger& UsbDeviceBase::Log() const
{
    return log;
}

//----------------------------------------------------------------------------------------------------------------------
// Device methods
//----------------------------------------------------------------------------------------------------------------------
bool UsbDeviceBase::MatchesTargetDevice(uint16_t vendorId, uint16_t productId) const
{
    return DddUsbProtocol::MatchesConfiguredDevice(targetDeviceVendorId, targetDeviceProductId,
        vendorId, productId);
}

//----------------------------------------------------------------------------------------------------------------------
bool UsbDeviceBase::IsPrimaryTargetDevice(uint16_t vendorId, uint16_t productId) const
{
    return vendorId == targetDeviceVendorId && productId == targetDeviceProductId;
}

//----------------------------------------------------------------------------------------------------------------------
bool UsbDeviceBase::ResolveTargetDevicePath(const std::string& preferredDevicePath, std::string& targetDevicePath) const
{
    std::vector<std::string> presentDevicePaths;
    if (!GetPresentDevicePaths(presentDevicePaths) || presentDevicePaths.empty())
    {
        return false;
    }

    targetDevicePath = presentDevicePaths.front();
    if (!preferredDevicePath.empty())
    {
        const auto preferredDevice = std::find(presentDevicePaths.begin(), presentDevicePaths.end(), preferredDevicePath);
        if (preferredDevice != presentDevicePaths.end())
        {
            targetDevicePath = *preferredDevice;
        }
        else if (presentDevicePaths.size() != 1)
        {
            // A firmware update can legitimately change the saved physical path. If
            // exactly one DDD is attached it is unambiguous and safe to adopt it; with
            // multiple devices, do not silently configure a different unit.
            return false;
        }
    }
    return true;
}

//----------------------------------------------------------------------------------------------------------------------
bool UsbDeviceBase::SendConfigurationCommand(const std::string& preferredDevicePath, bool testMode,
    uint8_t decimationFactor)
{
    if (transferInProgress)
    {
        Log().Error("SendConfigurationCommand(): Device configuration cannot change during capture");
        return false;
    }

    std::string targetDevicePath;
    if (!ResolveTargetDevicePath(preferredDevicePath, targetDevicePath))
    {
        Log().Error("SendConfigurationCommand(): Failed to locate a target device");
        return false;
    }
    return SendConfigurationCommandToDevice(targetDevicePath, testMode, decimationFactor);
}

//----------------------------------------------------------------------------------------------------------------------
bool UsbDeviceBase::SendConfigurationCommandToDevice(const std::string& targetDevicePath, bool testMode,
    uint8_t decimationFactor)
{
    configuredGatewareVersion.clear();
    if (!DddUsbProtocol::IsSupportedDecimationFactor(decimationFactor))
    {
        Log().Error("SendConfigurationCommand(): Unsupported decimation factor {0}",
            static_cast<unsigned int>(decimationFactor));
        configuredDeviceProtocol = DddUsbProtocol::DeviceProtocol::Unsupported;
        return false;
    }

    DddUsbProtocol::DeviceProtocol protocol = DddUsbProtocol::DeviceProtocol::Unsupported;
    if (!GetDeviceProtocol(targetDevicePath, protocol))
    {
        Log().Error("SendConfigurationCommand(): Failed to identify the selected device protocol");
        configuredDeviceProtocol = DddUsbProtocol::DeviceProtocol::Unsupported;
        return false;
    }

    switch (protocol)
    {
    case DddUsbProtocol::DeviceProtocol::Legacy:
        if (decimationFactor != DddUsbProtocol::FullRateDecimation)
        {
            Log().Error("SendConfigurationCommand(): 20 MSPS hardware decimation requires firmware 3.1 or later");
            configuredDeviceProtocol = DddUsbProtocol::DeviceProtocol::Unsupported;
            return false;
        }
        if (!SendVendorSpecificCommand(targetDevicePath,
            DddUsbProtocol::LegacyConfigurationRequest, testMode ? 1 : 0))
        {
            Log().Error("SendConfigurationCommand(): The legacy device rejected the configuration request");
            configuredDeviceProtocol = DddUsbProtocol::DeviceProtocol::Unsupported;
            return false;
        }
        configuredDeviceProtocol = protocol;
        return true;
    case DddUsbProtocol::DeviceProtocol::Version1:
    {
        std::vector<uint8_t> identity;
        if (!ReadDeviceRegisters(targetDevicePath, DddUsbProtocol::IdentityRegister,
            DddUsbProtocol::IdentityLength, identity) || identity.size() != DddUsbProtocol::IdentityLength)
        {
            Log().Error("SendConfigurationCommand(): Failed to read the firmware 3.1 FPGA identity block");
            configuredDeviceProtocol = DddUsbProtocol::DeviceProtocol::Unsupported;
            return false;
        }
        if (!DddUsbProtocol::IsSupportedApplicationIdentity(identity[DddUsbProtocol::IdentityRegister],
            identity[DddUsbProtocol::RegisterMapVersionRegister], identity[DddUsbProtocol::ImageRoleRegister]))
        {
            Log().Error("SendConfigurationCommand(): Unsupported or recovery FPGA image (identity {0}, map {1}, role {2})",
                static_cast<unsigned int>(identity[DddUsbProtocol::IdentityRegister]),
                static_cast<unsigned int>(identity[DddUsbProtocol::RegisterMapVersionRegister]),
                static_cast<unsigned int>(identity[DddUsbProtocol::ImageRoleRegister]));
            configuredDeviceProtocol = DddUsbProtocol::DeviceProtocol::Unsupported;
            return false;
        }

        if ((identity[DddUsbProtocol::BuildFlagsRegister] & DddUsbProtocol::BuildCommitFlag) != 0)
        {
            for (uint8_t index = 0; index < DddUsbProtocol::CommitLength; ++index)
            {
                const uint8_t character = identity[DddUsbProtocol::CommitRegister + index];
                const bool isHex = (character >= '0' && character <= '9') ||
                    (character >= 'a' && character <= 'f') ||
                    (character >= 'A' && character <= 'F');
                if (!isHex)
                    break;
                configuredGatewareVersion.push_back(static_cast<char>(character));
            }
            if (!configuredGatewareVersion.empty() &&
                (identity[DddUsbProtocol::BuildFlagsRegister] & DddUsbProtocol::BuildDirtyFlag) != 0)
            {
                configuredGatewareVersion += "-dirty";
            }
        }

        // Once a configuration write has been attempted, any later failure can leave
        // the gateware in test or half-rate mode even though capture never starts.
        // Restore the least surprising safe state on every partial-configuration path.
        const auto restoreSafeDefaults = [&]()
        {
            const bool testModeReset = SendVendorSpecificCommand(targetDevicePath,
                DddUsbProtocol::RegisterWriteRequest, DddUsbProtocol::MakeTestModeWrite(false));
            const bool sampleRateReset = SendVendorSpecificCommand(targetDevicePath,
                DddUsbProtocol::RegisterWriteRequest,
                DddUsbProtocol::MakeDecimationWrite(DddUsbProtocol::FullRateDecimation));
            std::vector<uint8_t> testModeReadback;
            std::vector<uint8_t> sampleRateReadback;
            const bool resetVerified = testModeReset && sampleRateReset &&
                ReadDeviceRegisters(targetDevicePath, DddUsbProtocol::TestModeRegister, 1,
                    testModeReadback) && testModeReadback.size() == 1 && testModeReadback[0] == 0 &&
                ReadDeviceRegisters(targetDevicePath, DddUsbProtocol::DecimationRegister, 1,
                    sampleRateReadback) && sampleRateReadback.size() == 1 &&
                sampleRateReadback[0] == DddUsbProtocol::FullRateDecimation;
            if (!resetVerified)
            {
                Log().Warning("SendConfigurationCommand(): Could not verify safe device defaults after a configuration error");
            }
        };
        if (!SendVendorSpecificCommand(targetDevicePath, DddUsbProtocol::RegisterWriteRequest,
            DddUsbProtocol::MakeTestModeWrite(testMode)))
        {
            Log().Error("SendConfigurationCommand(): Firmware 3.1 rejected the test-mode register write");
            restoreSafeDefaults();
            configuredDeviceProtocol = DddUsbProtocol::DeviceProtocol::Unsupported;
            return false;
        }
        if (!SendVendorSpecificCommand(targetDevicePath, DddUsbProtocol::RegisterWriteRequest,
            DddUsbProtocol::MakeDecimationWrite(decimationFactor)))
        {
            Log().Error("SendConfigurationCommand(): Firmware 3.1 rejected the sample-rate register write");
            restoreSafeDefaults();
            configuredDeviceProtocol = DddUsbProtocol::DeviceProtocol::Unsupported;
            return false;
        }

        // The gateware normalises unsupported values, so a successful control transfer is
        // not proof that the requested capture path took effect. Read both settings back
        // before B5 or the bulk endpoint is opened.
        std::vector<uint8_t> readback;
        const uint8_t expectedTestMode = testMode ? 1 : 0;
        if (!ReadDeviceRegisters(targetDevicePath, DddUsbProtocol::TestModeRegister, 1, readback) ||
            readback.size() != 1 || readback[0] != expectedTestMode)
        {
            Log().Error("SendConfigurationCommand(): Firmware 3.1 test-mode readback did not match {0}",
                static_cast<unsigned int>(expectedTestMode));
            restoreSafeDefaults();
            configuredDeviceProtocol = DddUsbProtocol::DeviceProtocol::Unsupported;
            return false;
        }
        readback.clear();
        if (!ReadDeviceRegisters(targetDevicePath, DddUsbProtocol::DecimationRegister, 1, readback) ||
            readback.size() != 1 || readback[0] != decimationFactor)
        {
            Log().Error("SendConfigurationCommand(): Firmware 3.1 decimation readback did not match {0}",
                static_cast<unsigned int>(decimationFactor));
            restoreSafeDefaults();
            configuredDeviceProtocol = DddUsbProtocol::DeviceProtocol::Unsupported;
            return false;
        }

        configuredDeviceProtocol = protocol;
        return true;
    }
    case DddUsbProtocol::DeviceProtocol::Unsupported:
        Log().Error("SendConfigurationCommand(): The selected device uses an unsupported USB protocol version");
        configuredDeviceProtocol = DddUsbProtocol::DeviceProtocol::Unsupported;
        return false;
    }

    return false;
}

//----------------------------------------------------------------------------------------------------------------------
// Capture methods
//----------------------------------------------------------------------------------------------------------------------
bool UsbDeviceBase::StartCapture(const std::filesystem::path& filePath, CaptureFormat format,
    const std::string& preferredDevicePath, bool isTestMode, bool useSmallUsbTransfers,
    bool useAsyncFileIo, size_t usbTransferQueueSizeInBytes,
    size_t diskBufferQueueSizeInBytes, int flacCompressionLevel,
    uint8_t decimationFactor, uint8_t frontEndGainSwitches)
{
    // If we're already performing a capture, abort any further processing.
    if (transferInProgress)
    {
        Log().Error("StartCapture(): Capture was currently in progress");
        return false;
    }

    // Resolve the selected physical path once and keep using it for every control request
    // and the streaming connection. If it disappears, fail instead of falling through to
    // another attached Duplicator part-way through the start sequence.
    std::string targetDevicePath;
    if (!ResolveTargetDevicePath(preferredDevicePath, targetDevicePath))
    {
        Log().Error("StartCapture(): Failed to locate the target device");
        captureResult = TransferResult::ConnectionFailure;
        return false;
    }

    // Configure the selected device immediately before capture. Legacy firmware uses the
    // original 0xB6 bit field; firmware 3.1 uses the protocol-v1 register write request.
    if (!SendConfigurationCommandToDevice(targetDevicePath, isTestMode, decimationFactor))
    {
        Log().Error("StartCapture(): Failed to configure the target device");
        captureResult = TransferResult::DeviceConfigurationError;
        return false;
    }

    // Protocol-v1 firmware uses 0xB5 to keep the USB 3 link out of U1/U2 while capture is
    // active. Without this, Windows can lose samples inside otherwise complete transfers.
    bool collectionStarted = false;
    if (configuredDeviceProtocol == DddUsbProtocol::DeviceProtocol::Version1)
    {
        if (!SendVendorSpecificCommand(targetDevicePath, DddUsbProtocol::CollectionRequest, 1))
        {
            Log().Error("StartCapture(): The target device rejected the collection start request");
            // A failed host-side transfer is ambiguous: the firmware may already have
            // acted on B5=1. B5=0 is idempotent, so always issue the compensating stop.
            SendVendorSpecificCommand(targetDevicePath, DddUsbProtocol::CollectionRequest, 0);
            SendConfigurationCommandToDevice(targetDevicePath, false,
                DddUsbProtocol::FullRateDecimation);
            captureResult = TransferResult::ConnectionFailure;
            return false;
        }
        collectionStarted = true;
    }

    // Any failure below must undo the protocol-v1 collection state. The existing capture
    // connection is closed first so the stop request can open the selected device cleanly.
    bool captureStartCommitted = false;
    std::shared_ptr<void> failedStartCleanup(nullptr,
        [&](void*)
        {
            if (captureStartCommitted)
            {
                return;
            }

            // An exception or late startup failure after flac was launched must
            // not leave a child process, inherited pipe, or joinable reader thread.
#ifdef _WIN32
            if (flacPipeHandle != nullptr)
            {
                flacStdinWriteHandle = INVALID_HANDLE_VALUE;
                fclose(flacPipeHandle);
                flacPipeHandle = nullptr;
            }
            bool flacProcessExited = true;
            if (flacPipeProcess != INVALID_HANDLE_VALUE)
            {
                flacProcessExited = WaitForSingleObject(flacPipeProcess, 5000) == WAIT_OBJECT_0;
                if (!flacProcessExited)
                {
                    TerminateProcess(flacPipeProcess, 1);
                    flacProcessExited = WaitForSingleObject(flacPipeProcess, 5000) == WAIT_OBJECT_0;
                }
                if (!flacProcessExited && flacErrorReaderThread.joinable())
                {
                    flacErrorReaderStopRequested = true;
                    const HANDLE readerThreadHandle = flacErrorReaderThreadHandle.load();
                    if (readerThreadHandle != nullptr)
                    {
                        CancelSynchronousIo(readerThreadHandle);
                    }
                }
                CloseHandle(flacPipeProcess);
                flacPipeProcess = INVALID_HANDLE_VALUE;
            }
            if (flacErrorReaderThread.joinable())
            {
                flacErrorReaderThread.join();
            }
            if (const HANDLE readerThreadHandle =
                    flacErrorReaderThreadHandle.exchange(nullptr);
                readerThreadHandle != nullptr)
            {
                CloseHandle(readerThreadHandle);
            }
            if (flacErrorReadPipeHandle != INVALID_HANDLE_VALUE)
            {
                CloseHandle(flacErrorReadPipeHandle);
                flacErrorReadPipeHandle = INVALID_HANDLE_VALUE;
            }
            if (windowsCaptureOutputFileHandle != INVALID_HANDLE_VALUE)
            {
                CloseHandle(windowsCaptureOutputFileHandle);
                windowsCaptureOutputFileHandle = INVALID_HANDLE_VALUE;
            }
#else
            if (flacPipeHandle != nullptr)
            {
                fclose(flacPipeHandle);
                flacPipeHandle = nullptr;
            }
            if (flacPipeProcessId > 0)
            {
                int processStatus = 0;
                if (!waitForFlacProcess(static_cast<pid_t>(flacPipeProcessId), 5000,
                    processStatus))
                {
                    terminateFlacProcess(static_cast<pid_t>(flacPipeProcessId),
                        processStatus);
                }
                flacPipeProcessId = -1;
            }
#endif
            if (captureOutputFile.is_open())
            {
                captureOutputFile.close();
            }
            if (format == CaptureFormat::Signed16BitFlacOnTheFly)
            {
                std::error_code removeError;
                std::filesystem::remove(filePath, removeError);
            }
            if (DeviceConnected())
            {
                DisconnectFromDevice();
            }
            if (collectionStarted)
            {
                SendVendorSpecificCommand(targetDevicePath, DddUsbProtocol::CollectionRequest, 0);
            }
            SendConfigurationCommandToDevice(targetDevicePath, false,
                DddUsbProtocol::FullRateDecimation);
#ifdef _WIN32
            if (useWindowsOverlappedFileIo && diskBufferEntries)
            {
                for (size_t index = 0; index < totalDiskBufferEntryCount; ++index)
                {
                    HANDLE eventHandle = diskBufferEntries[index].diskWriteOverlappedBuffer.hEvent;
                    if (eventHandle != nullptr && eventHandle != INVALID_HANDLE_VALUE)
                    {
                        CloseHandle(eventHandle);
                        diskBufferEntries[index].diskWriteOverlappedBuffer.hEvent = nullptr;
                    }
                }
            }
#endif
            diskBufferEntries.reset();
            transferInProgress = false;
        });

    // Attempt to connect to the target device
    if (!ConnectToDevice(targetDevicePath))
    {
        Log().Error("StartCapture(): Failed to connect to the target device");
        captureResult = TransferResult::ConnectionFailure;
        return false;
    }

    // Flag whether we should be using asynchronous IO (Windows only)
#ifdef _WIN32
    useWindowsOverlappedFileIo = useAsyncFileIo;
#endif

    // Attempt to create/open the output file or pipe
    if (format == CaptureFormat::Signed16BitFlacOnTheFly)
    {
        // Firmware 3.1 has already performed any requested 2:1 half-band decimation.
        // Feed the exact signed-16 mapping directly to native FLAC: no host resampling
        // and no 8-bit conversion. flac owns the target file so it can seek back at EOF
        // and finalise STREAMINFO (sample count and MD5).
        const int level = std::clamp(flacCompressionLevel, 0, 8);
        const unsigned int flacThreads = std::clamp(std::thread::hardware_concurrency(), 1U, 8U);
        const uint32_t realSampleRate = DddUsbProtocol::SampleRateInHzForDecimation(decimationFactor);
        const uint32_t flacSampleRate = DddUsbProtocol::FlacSampleRateLabelForDecimation(decimationFactor);
        const uint8_t declaredGain = DddFrontEndGain::NormalizeSwitchPattern(frontEndGainSwitches);
        const std::string gainDescription = DddFrontEndGain::Description(declaredGain);

#ifdef _WIN32
        {
            std::wstring flacExecutable = L"flac.exe";
            wchar_t wExePath[MAX_PATH] = {};
            GetModuleFileNameW(NULL, wExePath, MAX_PATH);
            std::wstring exeDirW(wExePath);
            auto lastSlash = exeDirW.find_last_of(L"\\/");
            exeDirW = (lastSlash != std::wstring::npos) ? exeDirW.substr(0, lastSlash + 1) : L"";
            const std::wstring localFlac = exeDirW + L"flac.exe";
            if (std::filesystem::exists(localFlac))
            {
                flacExecutable = localFlac;
            }
            else
            {
                // CreateProcessW does not search PATH when a relative executable is
                // supplied as lpApplicationName. Resolve it first so installed flac.exe
                // distributions work even when the binary is not bundled beside us.
                std::vector<wchar_t> resolvedPath(32768);
                const DWORD resolvedLength = SearchPathW(nullptr, L"flac.exe", nullptr,
                    static_cast<DWORD>(resolvedPath.size()), resolvedPath.data(), nullptr);
                if (resolvedLength > 0 && resolvedLength < resolvedPath.size())
                {
                    flacExecutable.assign(resolvedPath.data(), resolvedLength);
                }
            }

            std::wstring arguments = L"-" + std::to_wstring(level)
                + L" -j " + std::to_wstring(flacThreads)
                + L" --bps=16 --sign=signed --channels=1 --endian=little"
                + L" --sample-rate=" + std::to_wstring(flacSampleRate)
                + L" --no-seektable --force-raw-format -f"
                + L" --tag=ENCODER=DomesdayDuplicator-2.1"
                + L" --tag=DDD_VERSION=2.1"
                + L" --tag=DDD_SAMPLE_RATE_HZ=" + std::to_wstring(realSampleRate)
                + L" --tag=DDD_DECIMATION=" + std::to_wstring(decimationFactor)
                + L" --tag=DDD_TEST_MODE=" + std::wstring(isTestMode ? L"true" : L"false");
            if (!configuredGatewareVersion.empty())
            {
                arguments += L" --tag=DDD_GATEWARE_VERSION=" +
                    std::wstring(configuredGatewareVersion.begin(), configuredGatewareVersion.end());
            }
            if (!gainDescription.empty())
            {
                arguments += L" --tag=\"DDD_FRONT_END_GAIN=" +
                    utf8ToWide(gainDescription) + L"\"";
            }
            arguments += L" -o \"" + filePath.wstring() + L"\" -";

            Log().Info("StartCapture(): Starting native signed-16 FLAC encoder");
            flacErrorOutput.clear();
            flacErrorReaderStopRequested = false;
            flacErrorReaderThreadHandle = nullptr;
            flacPipeHandle = openFlacPipeNoWindow(flacExecutable, arguments,
                flacPipeProcess, flacErrorReadPipeHandle);
            // Store the raw HANDLE so ProcessingThread can use WriteFile directly,
            // bypassing the MinGW CRT which may call abort() on a broken pipe.
            if (flacPipeHandle != nullptr)
            {
                flacStdinWriteHandle = reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(flacPipeHandle)));
                flacErrorReaderThread = std::thread([this]() {
                    // std::thread::native_handle() is a Win32 HANDLE under MSVC but a
                    // pthread identifier under MinGW. Duplicate the current pseudo-handle
                    // into a real Win32 thread HANDLE that CancelSynchronousIo accepts on
                    // both supported Windows toolchains.
                    HANDLE readerThreadHandle = nullptr;
                    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(),
                        GetCurrentProcess(), &readerThreadHandle, THREAD_TERMINATE,
                        FALSE, 0))
                    {
                        Log().Warning("StartCapture(): Could not create a cancellable FLAC diagnostic-reader handle");
                    }
                    flacErrorReaderThreadHandle = readerThreadHandle;

                    const DWORD bufferSize = 4096;
                    std::vector<char> buffer(bufferSize);
                    DWORD bytesRead = 0;
                    while (!flacErrorReaderStopRequested &&
                        ReadFile(flacErrorReadPipeHandle, buffer.data(), bufferSize,
                        &bytesRead, nullptr) && bytesRead > 0)
                    {
                        constexpr size_t maximumDiagnosticBytes = 256 * 1024;
                        flacErrorOutput.append(buffer.data(), bytesRead);
                        if (flacErrorOutput.size() > maximumDiagnosticBytes)
                        {
                            flacErrorOutput.erase(0,
                                flacErrorOutput.size() - maximumDiagnosticBytes);
                        }
                    }
                });

                // flac 1.5 is required for -j. Bad options and unwritable output
                // paths cause the child to exit immediately; detect that before USB
                // capture threads start so the device can be cleanly rolled back.
                const DWORD startupWait = WaitForSingleObject(flacPipeProcess, 150);
                if (startupWait == WAIT_OBJECT_0 || startupWait == WAIT_FAILED)
                {
                    const DWORD waitError = startupWait == WAIT_FAILED ? GetLastError() : ERROR_SUCCESS;
                    flacStdinWriteHandle = INVALID_HANDLE_VALUE;
                    fclose(flacPipeHandle);
                    flacPipeHandle = nullptr;
                    if (startupWait == WAIT_FAILED)
                    {
                        TerminateProcess(flacPipeProcess, 1);
                        if (WaitForSingleObject(flacPipeProcess, 5000) != WAIT_OBJECT_0 &&
                            flacErrorReaderThread.joinable())
                        {
                            flacErrorReaderStopRequested = true;
                            const HANDLE readerThreadHandle = flacErrorReaderThreadHandle.load();
                            if (readerThreadHandle != nullptr)
                            {
                                CancelSynchronousIo(readerThreadHandle);
                            }
                        }
                    }
                    DWORD exitCode = 1;
                    GetExitCodeProcess(flacPipeProcess, &exitCode);
                    CloseHandle(flacPipeProcess);
                    flacPipeProcess = INVALID_HANDLE_VALUE;
                    if (flacErrorReaderThread.joinable())
                    {
                        flacErrorReaderThread.join();
                    }
                    if (const HANDLE readerThreadHandle =
                            flacErrorReaderThreadHandle.exchange(nullptr);
                        readerThreadHandle != nullptr)
                    {
                        CloseHandle(readerThreadHandle);
                    }
                    if (flacErrorReadPipeHandle != INVALID_HANDLE_VALUE)
                    {
                        CloseHandle(flacErrorReadPipeHandle);
                        flacErrorReadPipeHandle = INVALID_HANDLE_VALUE;
                    }
                    if (!flacErrorOutput.empty())
                    {
                        Log().Error(std::string("FLAC encoder startup output: ") + flacErrorOutput);
                    }
                    if (startupWait == WAIT_FAILED)
                    {
                        Log().Error("StartCapture(): Failed while checking the FLAC encoder process (error {0})",
                            waitError);
                    }
                    else
                    {
                        Log().Error("StartCapture(): FLAC encoder exited during startup with code {0}", exitCode);
                    }
                    captureResult = TransferResult::FileCreationError;
                    return false;
                }
            }
        }
#else
        {
            std::string flacCmd   = "flac";
#ifdef __APPLE__
            // On macOS, look for flac next to our own executable first.
            // When running from an .app bundle they live in Contents/MacOS/ next to the main binary.
            {
                char execPath[4096] = {};
                uint32_t pathSize = sizeof(execPath);
                if (_NSGetExecutablePath(execPath, &pathSize) == 0)
                {
                    std::filesystem::path execDir = std::filesystem::path(execPath).parent_path();
                    std::filesystem::path flacLocal   = execDir / "flac";
                    if (std::filesystem::exists(flacLocal))   flacCmd   = "\"" + flacLocal.string() + "\"";
                }
            }
#endif
            // On non-Windows the managed child pipe only carries command stdin.
            // Pass "-o path" so flac writes and finalises the target file directly.
            // Shell-quote the path: wrap in single quotes, escaping embedded single quotes
            // as '\'' (close-quote, escaped-quote, re-open-quote) — handles all filenames.
            std::string quotedOutputPath = "'";
            for (char c : filePath.string()) {
                if (c == '\'') quotedOutputPath += "'\\''";
                else           quotedOutputPath += c;
            }
            quotedOutputPath += "'";

            std::string cmd = flacCmd + " -" + std::to_string(level)
                + " -j " + std::to_string(flacThreads)
                + " --bps=16 --sign=signed --channels=1 --endian=little "
                + "--sample-rate=" + std::to_string(flacSampleRate) + " "
                + "--no-seektable --force-raw-format -f "
                + "--tag='ENCODER=DomesdayDuplicator-2.1' "
                + "--tag='DDD_VERSION=2.1' "
                + "--tag='DDD_SAMPLE_RATE_HZ=" + std::to_string(realSampleRate) + "' "
                + "--tag='DDD_DECIMATION=" + std::to_string(decimationFactor) + "' "
                + "--tag='DDD_TEST_MODE=" + std::string(isTestMode ? "true" : "false") + "' ";
            if (!configuredGatewareVersion.empty())
            {
                cmd += "--tag='DDD_GATEWARE_VERSION=" + configuredGatewareVersion + "' ";
            }
            if (!gainDescription.empty())
            {
                cmd += "--tag='DDD_FRONT_END_GAIN=" + gainDescription + "' ";
            }
            cmd += "-o " + quotedOutputPath + " -";
            Log().Info("StartCapture(): Starting native signed-16 FLAC encoder");
            flacPipeHandle = openFlacPipeProcess(cmd, flacPipeProcessId);
            if (flacPipeHandle != nullptr)
            {
                // fork()/exec is asynchronous. Catch a missing/incompatible encoder or
                // an immediately rejected output path before capture threads are started.
                int startupStatus = 0;
                if (waitForFlacProcess(static_cast<pid_t>(flacPipeProcessId), 150,
                    startupStatus))
                {
                    fclose(flacPipeHandle);
                    flacPipeHandle = nullptr;
                    flacPipeProcessId = -1;
                    if (WIFEXITED(startupStatus))
                    {
                        Log().Error("StartCapture(): FLAC encoder exited during startup with code {0}",
                            WEXITSTATUS(startupStatus));
                    }
                    else
                    {
                        Log().Error("StartCapture(): FLAC encoder terminated during startup");
                    }
                    captureResult = TransferResult::FileCreationError;
                    return false;
                }
            }
        }
#endif
        if (flacPipeHandle == nullptr)
        {
            Log().Error("StartCapture(): Failed to open FLAC pipe");
            captureResult = TransferResult::FileCreationError;
            return false;
        }
    }
    else
    {
#ifdef _WIN32
        if (useWindowsOverlappedFileIo)
        {
            windowsCaptureOutputFileHandle = CreateFileW(filePath.wstring().c_str(), GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_WRITE_THROUGH | FILE_FLAG_OVERLAPPED, NULL);
            if (windowsCaptureOutputFileHandle == INVALID_HANDLE_VALUE)
            {
                DWORD lastError = GetLastError();
                Log().Error("CreateFileW returned {0} with error code {1}.", windowsCaptureOutputFileHandle, lastError);
                captureResult = TransferResult::FileCreationError;
                return false;
            }
        }
        else
        {
#endif
#ifndef __APPLE__
            captureOutputFile.clear();
            captureOutputFile.rdbuf()->pubsetbuf(0, 0);
            captureOutputFile.open(filePath, std::ios::out | std::ios::trunc | std::ios::binary);
            if (!captureOutputFile.is_open())
            {
                Log().Error("StartCapture(): Failed to create the output file at path {0}", filePath);
                captureResult = TransferResult::FileCreationError;
                return false;
            }
#endif
#ifdef _WIN32
        }
#endif
#ifdef __APPLE__
        // On macOS, bypass the page cache to prevent dirty-page write throttling.
        // std::ofstream routes through the page cache and gets throttled to ~13 MB/s when
        // dirty pages accumulate, which is slower than the 80 MB/s USB input rate.
        // open() + F_NOCACHE writes directly to disk at full disk speed (~100+ MB/s),
        // which is faster than the USB input rate and allows indefinite capture.
        macosOutputFd = ::open(filePath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (macosOutputFd < 0)
        {
            Log().Error("StartCapture(): Failed to create output file at path {0} (errno {1})", filePath, errno);
            captureResult = TransferResult::FileCreationError;
            return false;
        }
        if (::fcntl(macosOutputFd, F_NOCACHE, 1) < 0)
        {
            Log().Warning("StartCapture(): F_NOCACHE not available, page-cache throttling may cause capture stalls");
        }

        // Launch the off-thread writer so the processing thread never blocks on disk I/O
        macosWriteFillBuffer.clear();
        macosWriteFillBuffer.reserve(macosWriteChunkBytes);
        macosWriteQueue.clear();
        macosWriteThreadExit = false;
        macosWriteError.store(false);
        macosWriteThread = std::thread(std::bind(std::mem_fn(&UsbDeviceBase::MacosWriteThread), this));
#endif
    }

    // Calculate the optimal read buffer size and number of disk buffers, and initialize the structures. We use an
    // unusual case of wrapping an array new into a unique_ptr rather than std::vector here, as we have an atomic_flag
    // member in the structure which can't be moved.
    try
    {
        CalculateDesiredBufferCountAndSize(useSmallUsbTransfers, usbTransferQueueSizeInBytes,
            diskBufferQueueSizeInBytes, totalDiskBufferEntryCount, diskBufferSizeInBytes);
        diskBufferEntries.reset(new DiskBufferEntry[totalDiskBufferEntryCount]);
        for (size_t i = 0; i < totalDiskBufferEntryCount; ++i)
        {
            DiskBufferEntry& entry = diskBufferEntries[i];
            entry.readBuffer.resize(diskBufferSizeInBytes);
            entry.isDiskBufferFull.clear();
#ifdef _WIN32
            entry.diskWriteInProgress = false;
            if (useWindowsOverlappedFileIo)
            {
                entry.diskWriteOverlappedBuffer = {};
                entry.diskWriteOverlappedBuffer.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
                if (entry.diskWriteOverlappedBuffer.hEvent == nullptr)
                    throw std::runtime_error("CreateEventW failed for a disk buffer");
            }
#endif
        }
    }
    catch (const std::bad_alloc&)
    {
        Log().Error("StartCapture(): Not enough memory for the configured capture buffers");
        captureResult = TransferResult::UsbMemoryLimit;
        return false;
    }
    catch (const std::exception& exception)
    {
        Log().Error("StartCapture(): Failed to initialize capture buffers: {0}", exception.what());
        captureResult = TransferResult::ProgramError;
        return false;
    }

    // Record the capture settings
    captureFilePath = filePath;
    captureFormat = format;
    captureIsTestMode = isTestMode;
    currentHardwareDecimationFactor = decimationFactor;
    currentCaptureSampleRateInHz = DddUsbProtocol::SampleRateInHzForDecimation(decimationFactor);
    captureFrontEndGainSwitches = DddFrontEndGain::NormalizeSwitchPattern(frontEndGainSwitches);
    currentUsbTransferQueueSizeInBytes = usbTransferQueueSizeInBytes;
    currentUseSmallUsbTransfers = useSmallUsbTransfers;

    // Initialize capture status
    transferInProgress = true;
    captureResult = TransferResult::Running;
    transferCount = 0;
    transferBufferWrittenCount = 0;
    transferFileSizeWrittenInBytes = 0;
    minSampleValue = std::numeric_limits<decltype(minSampleValue.load())>::max();
    maxSampleValue = 0;
    clippedMinSampleCount = 0;
    clippedMaxSampleCount = 0;
    captureThreadStopRequested.clear();
    captureThreadRunning.test_and_set();
    captureThreadRunning.notify_all();

    // Initialize our sequence/test data check state
    sequenceState = SequenceState::Sync;
    savedSequenceCounter = 0;
    expectedNextTestDataValue.reset();
    testDataMax.reset();

    capturePreferredDevicePath = targetDevicePath;
    currentFirmwareCollectionActive = collectionStarted;

    // Spin up a thread to handle the execution of the capture process from here on
    try
    {
        std::thread captureThread(std::bind(std::mem_fn(&UsbDeviceBase::CaptureThread), this));
        captureThread.detach();
    }
    catch (const std::exception& exception)
    {
        Log().Error("StartCapture(): Failed to create the capture thread: {0}", exception.what());
        captureResult = TransferResult::ProgramError;
        captureThreadRunning.clear();
        captureThreadRunning.notify_all();
        return false;
    }
    captureStartCommitted = true;
    return true;
}

//----------------------------------------------------------------------------------------------------------------------
void UsbDeviceBase::StopCapture()
{
    // If a transfer isn't currently in progress, abort any further processing.
    if (!transferInProgress)
    {
        Log().Error("StopCapture(): No capture was currently in progress");
        return;
    }

    // Instruct the capture thread to terminate, and wait for confirmation that it has stopped.
    captureThreadStopRequested.test_and_set();
    captureThreadStopRequested.notify_all();
    std::mutex flacDrainMutex;
    std::condition_variable flacDrainCondition;
    bool captureThreadStopped = false;
    std::thread flacDrainWatchdog;
    bool flacProcessAvailable = false;
#ifdef _WIN32
    flacProcessAvailable = flacPipeProcess != INVALID_HANDLE_VALUE;
#else
    flacProcessAvailable = flacPipeProcessId > 0;
#endif
    if (captureFormat == CaptureFormat::Signed16BitFlacOnTheFly && flacProcessAvailable)
    {
        flacDrainWatchdog = std::thread([&]()
        {
            std::unique_lock<std::mutex> lock(flacDrainMutex);
            if (!flacDrainCondition.wait_for(lock, std::chrono::seconds(30),
                [&]() { return captureThreadStopped; }))
            {
                Log().Error("StopCapture(): Timed out draining samples into the FLAC encoder");
#ifdef _WIN32
                // If flac stopped reading, ProcessingThread may be blocked in a
                // synchronous WriteFile. Killing the child closes the pipe's read end;
                // CancelIoEx is a second unblocking path if process termination fails.
                if (!TerminateProcess(flacPipeProcess, 1) && GetLastError() != ERROR_ACCESS_DENIED)
                {
                    Log().Error("StopCapture(): Failed to stop the stalled FLAC encoder (error {0})",
                        GetLastError());
                }
                if (flacStdinWriteHandle != INVALID_HANDLE_VALUE)
                {
                    CancelIoEx(flacStdinWriteHandle, nullptr);
                }
#else
                // Closing the encoder process group's read end turns a blocked fwrite
                // into EPIPE. ProcessingThread blocks SIGPIPE so it reports FileWriteError
                // instead of terminating the application.
                signalFlacProcessGroup(static_cast<pid_t>(flacPipeProcessId), SIGKILL);
#endif
            }
        });
    }
    captureThreadRunning.wait(true);
    {
        std::lock_guard<std::mutex> lock(flacDrainMutex);
        captureThreadStopped = true;
    }
    flacDrainCondition.notify_one();
    if (flacDrainWatchdog.joinable())
    {
        flacDrainWatchdog.join();
    }

    // Release our memory holding the disk buffers
#ifdef _WIN32
    if (useWindowsOverlappedFileIo)
    {
        for (size_t i = 0; i < totalDiskBufferEntryCount; ++i)
        {
            DiskBufferEntry& entry = diskBufferEntries[i];
            CloseHandle(entry.diskWriteOverlappedBuffer.hEvent);
        }
    }
#endif
    diskBufferEntries.reset();

    // Close the output file or pipe
    if (captureFormat == CaptureFormat::Signed16BitFlacOnTheFly)
    {
        bool encoderFinishedSuccessfully = true;
        if (flacPipeHandle != nullptr)
        {
#ifdef _WIN32
            // Close stdin to deliver EOF. flac then finishes its last frame and seeks back
            // to patch STREAMINFO before it exits.
            flacStdinWriteHandle = INVALID_HANDLE_VALUE; // fclose below closes the underlying HANDLE
            if (fclose(flacPipeHandle) != 0)
            {
                encoderFinishedSuccessfully = false;
            }
            flacPipeHandle = nullptr;
            if (flacPipeProcess != INVALID_HANDLE_VALUE)
            {
                constexpr DWORD encoderFinishTimeoutInMs = 30000;
                const DWORD waitResult = WaitForSingleObject(flacPipeProcess, encoderFinishTimeoutInMs);
                bool processExited = waitResult == WAIT_OBJECT_0;
                if (waitResult == WAIT_TIMEOUT)
                {
                    Log().Error("StopCapture(): FLAC encoder did not finish within 30 seconds");
                    encoderFinishedSuccessfully = false;
                }
                else if (waitResult == WAIT_FAILED)
                {
                    Log().Error("StopCapture(): Failed while waiting for the FLAC encoder (error {0})",
                        GetLastError());
                    encoderFinishedSuccessfully = false;
                }

                if (!processExited)
                {
                    if (!TerminateProcess(flacPipeProcess, 1))
                    {
                        Log().Error("StopCapture(): Failed to terminate the FLAC encoder (error {0})",
                            GetLastError());
                    }
                    const DWORD terminationWait = WaitForSingleObject(flacPipeProcess, 5000);
                    processExited = terminationWait == WAIT_OBJECT_0;
                    if (!processExited)
                    {
                        Log().Error("StopCapture(): FLAC encoder remained active after termination request");
                        encoderFinishedSuccessfully = false;
                        // The child may still own the diagnostic pipe's write end. Cancel
                        // the reader's blocking ReadFile so joining it cannot hang forever.
                        if (flacErrorReaderThread.joinable())
                        {
                            flacErrorReaderStopRequested = true;
                            const HANDLE readerThreadHandle = flacErrorReaderThreadHandle.load();
                            if (readerThreadHandle != nullptr)
                            {
                                CancelSynchronousIo(readerThreadHandle);
                            }
                        }
                    }
                }

                DWORD exitCode = STILL_ACTIVE;
                if (!GetExitCodeProcess(flacPipeProcess, &exitCode) ||
                    exitCode == STILL_ACTIVE || exitCode != 0)
                {
                    Log().Error("StopCapture(): FLAC encoder exited with code {0}", exitCode);
                    encoderFinishedSuccessfully = false;
                }
                CloseHandle(flacPipeProcess);
                flacPipeProcess = INVALID_HANDLE_VALUE;
            }
            if (flacErrorReaderThread.joinable())
            {
                flacErrorReaderThread.join();
            }
            if (const HANDLE readerThreadHandle =
                    flacErrorReaderThreadHandle.exchange(nullptr);
                readerThreadHandle != nullptr)
            {
                CloseHandle(readerThreadHandle);
            }
            if (flacErrorReadPipeHandle != INVALID_HANDLE_VALUE)
            {
                CloseHandle(flacErrorReadPipeHandle);
                flacErrorReadPipeHandle = INVALID_HANDLE_VALUE;
            }
            if (!flacErrorOutput.empty())
            {
                if (encoderFinishedSuccessfully)
                {
                    Log().Debug(std::string("FLAC encoder output: ") + flacErrorOutput);
                }
                else
                {
                    Log().Error(std::string("FLAC encoder output: ") + flacErrorOutput);
                }
            }
#else
            if (fclose(flacPipeHandle) != 0)
            {
                Log().Error("StopCapture(): Failed to close the FLAC encoder input pipe");
                encoderFinishedSuccessfully = false;
            }
            flacPipeHandle = nullptr;
            if (flacPipeProcessId > 0)
            {
                int encoderStatus = 0;
                bool processExited = waitForFlacProcess(
                    static_cast<pid_t>(flacPipeProcessId), 30000, encoderStatus);
                if (!processExited)
                {
                    Log().Error("StopCapture(): FLAC encoder did not finish within 30 seconds");
                    processExited = terminateFlacProcess(
                        static_cast<pid_t>(flacPipeProcessId), encoderStatus);
                }
                if (!processExited || !WIFEXITED(encoderStatus) || WEXITSTATUS(encoderStatus) != 0)
                {
                    Log().Error("StopCapture(): FLAC encoder did not exit successfully");
                    encoderFinishedSuccessfully = false;
                }
                flacPipeProcessId = -1;
            }
#endif
        }

        // A zero process exit is necessary but not sufficient: verify that the final file
        // exists and is native FLAC rather than an empty/truncated output.
        std::error_code fileSizeError;
        const uintmax_t finalFileSize = std::filesystem::file_size(captureFilePath, fileSizeError);
        if (!fileSizeError)
        {
            transferFileSizeWrittenInBytes = static_cast<size_t>(finalFileSize);
        }
        std::ifstream flacFile(captureFilePath, std::ios::in | std::ios::binary);
        char flacMagic[4] = {};
        flacFile.read(flacMagic, sizeof(flacMagic));
        if (!flacFile || std::string(flacMagic, sizeof(flacMagic)) != "fLaC")
        {
            Log().Error("StopCapture(): FLAC encoder did not produce a valid native FLAC header");
            encoderFinishedSuccessfully = false;
        }
        if (!encoderFinishedSuccessfully && captureResult == TransferResult::Success)
        {
            captureResult = TransferResult::FileWriteError;
        }
    }
    else
    {
#ifdef _WIN32
        if (useWindowsOverlappedFileIo)
        {
            BOOL closeHandleReturn = CloseHandle(windowsCaptureOutputFileHandle);
            if (closeHandleReturn == 0)
            {
                DWORD lastError = GetLastError();
                Log().Error("CloseHandle failed with error code {0}.", lastError);
            }
            windowsCaptureOutputFileHandle = INVALID_HANDLE_VALUE;
        }
        else
        {
#endif
#ifdef __APPLE__
            // Flush any remaining data in the fill buffer and stop the write thread before
            // closing the file, so all data is written in order.
            if (macosWriteThread.joinable())
            {
                {
                    std::unique_lock<std::mutex> lock(macosWriteMutex);
                    if (!macosWriteFillBuffer.empty())
                    {
                        macosWriteQueue.push_back(std::move(macosWriteFillBuffer));
                        macosWriteFillBuffer.clear();
                    }
                    macosWriteThreadExit = true;
                }
                macosWriteCv.notify_all();
                macosWriteThread.join();
            }
            if (macosOutputFd >= 0)
            {
                ::close(macosOutputFd);
                macosOutputFd = -1;
            }
#else
            captureOutputFile.close();
#endif
#ifdef _WIN32
        }
#endif
    }

    // Disconnect from the target device
    DisconnectFromDevice();

    // Tell protocol-v1 firmware that capture is over only after the streaming handle has
    // been released. This restores normal U1/U2 link power management on the device.
    if (currentFirmwareCollectionActive)
    {
        if (!SendVendorSpecificCommand(capturePreferredDevicePath, DddUsbProtocol::CollectionRequest, 0))
        {
            Log().Warning("StopCapture(): Failed to send the collection stop request");
        }
        currentFirmwareCollectionActive = false;
    }
    capturePreferredDevicePath.clear();

    // Record that the capture process has completed
    Log().Info("StopCapture(): Ended capture process");
    transferInProgress = false;
}

//----------------------------------------------------------------------------------------------------------------------
void UsbDeviceBase::CaptureThread()
{
  try
  {
    // Determine how large our conversion buffers need to be based on the disk buffer size and the capture format
    size_t requiredConversionBufferSize = 0;
    switch (captureFormat)
    {
    case CaptureFormat::Signed16Bit:
        requiredConversionBufferSize = diskBufferSizeInBytes;
        break;
    case CaptureFormat::Signed16BitHalf:
        // Downsampled by 2, so output will be approximately half the size
        requiredConversionBufferSize = diskBufferSizeInBytes / 2;
        break;
    case CaptureFormat::Signed16BitQuarter:
        // Downsampled by 4, so output will be approximately quarter the size
        requiredConversionBufferSize = diskBufferSizeInBytes / 4;
        break;
    case CaptureFormat::Unsigned10Bit:
        requiredConversionBufferSize = (diskBufferSizeInBytes / 8) * 5;
        break;
    case CaptureFormat::Unsigned10Bit4to1Decimation:
        requiredConversionBufferSize = (diskBufferSizeInBytes / (8 * 4)) * 5;
        break;
    case CaptureFormat::Signed16BitFlacOnTheFly:
        // The FPGA-selected 20/40 MSPS stream is mapped losslessly to signed-16.
        requiredConversionBufferSize = diskBufferSizeInBytes;
        break;
    }

    // Allocate our conversion buffers
    for (size_t i = 0; i < conversionBufferCount; ++i)
    {
        conversionBuffers[i].resize(requiredConversionBufferSize);
    }

    // Lock all the critical structures into physical memory. This stops these buffers getting paged out, which could
    // cause page faults and lead to missed data packets.
    for (size_t i = 0; i < conversionBufferCount; ++i)
    {
        LockMemoryBufferIntoPhysicalMemory(conversionBuffers[i].data(), conversionBuffers[i].size());
    }
    for (size_t i = 0; i < totalDiskBufferEntryCount; ++i)
    {
        DiskBufferEntry& entry = diskBufferEntries[i];
        LockMemoryBufferIntoPhysicalMemory(entry.readBuffer.data(), entry.readBuffer.size());
    }
    LockMemoryBufferIntoPhysicalMemory(diskBufferEntries.get(), sizeof(diskBufferEntries[0]) * totalDiskBufferEntryCount);
    LockMemoryBufferIntoPhysicalMemory(this, sizeof(*this));

    // Attempt to boost the process priority to realtime
    boostedProcessPriority = SetCurrentProcessRealtimePriority(processPriorityRestoreInfo);
    if (!boostedProcessPriority)
    {
        Log().Warning("CaptureThread(): Failed to boost process priority");
    }

    // Record that a capture process is starting
    Log().Info("CaptureThread(): Starting capture process");

    usbTransferRunning.test_and_set();
    processingRunning.test_and_set();
    usbTransferStopRequested.clear();
    processingStopRequested.clear();
    dumpAllCaptureDataInProgress.clear();
    usbTransferResult = TransferResult::Running;
    processingResult = TransferResult::Running;

    // Start a worker thread to process data after it's read
    std::thread processingThread(std::bind(std::mem_fn(&UsbDeviceBase::ProcessingThread), this));

    // Start a worker thread to transfer data from the USB device
    std::thread usbTransferThread(std::bind(std::mem_fn(&UsbDeviceBase::UsbTransferThread), this));

    // Run transfer continously until we're signalled to stop for some reason
    captureThreadStopRequested.wait(false);

    // Wind up the capture process, latching the appropriate result if an error has occurred.
    TransferResult result = TransferResult::ProgramError;
    bool errorCodeLatched = false;
    bool usbTransferFailed = false;
    bool usbTransferResultChecked = false;
    bool processingFailed = false;
    bool processingResultChecked = false;
    while (usbTransferRunning.test() || processingRunning.test())
    {
        // Check for a USB transfer failure
        if (!usbTransferResultChecked && !usbTransferRunning.test())
        {
            auto transferResultTemp = usbTransferResult.load();
            if (transferResultTemp == TransferResult::Running)
            {
                if (!errorCodeLatched)
                {
                    result = TransferResult::ProgramError;
                    errorCodeLatched = true;
                }
                usbTransferFailed = true;
            }
            else if (transferResultTemp != TransferResult::Success)
            {
                if (!errorCodeLatched)
                {
                    result = transferResultTemp;
                    errorCodeLatched = true;
                }
                usbTransferFailed = true;
            }
            usbTransferResultChecked = true;
        }

        // Check for a data processing failure
        if (!processingResultChecked && !processingRunning.test())
        {
            auto transferResultTemp = processingResult.load();
            if (transferResultTemp == TransferResult::Running)
            {
                if (!errorCodeLatched)
                {
                    result = TransferResult::ProgramError;
                    errorCodeLatched = true;
                }
                processingFailed = true;
            }
            else if (transferResultTemp != TransferResult::Success)
            {
                if (!errorCodeLatched)
                {
                    result = transferResultTemp;
                    errorCodeLatched = true;
                }
                processingFailed = true;
            }
            processingResultChecked = true;
        }

        // Request the USB transfer child worker thread to stop. If no errors have occurred or occur during the final
        // stages, this should cause it to emit all remaining queued transfers and stop gracefully.
        usbTransferStopRequested.test_and_set();
        usbTransferStopRequested.notify_all();

        // Request the processing child worker thread to stop if the USB transfer worker thread has completed, or if an
        // error has occurred. This should cause it to process all queued disk buffers and stop gracefully. We pad out
        // any empty disk buffers as being dumped, and signal their completion, to unblock the worker thread in case
        // it's currently waiting on the next buffer.
        if (!usbTransferRunning.test() || usbTransferFailed || processingFailed)
        {
            processingStopRequested.test_and_set();
            processingStopRequested.notify_all();
#ifdef __APPLE__
            macosWriteCv.notify_all(); // wake processing thread if blocked waiting for write queue space
#endif
            for (size_t i = 0; i < totalDiskBufferEntryCount; ++i)
            {
                DiskBufferEntry& entry = diskBufferEntries[i];
                if (!entry.isDiskBufferFull.test())
                {
                    entry.dumpingBuffer.test_and_set();
                    entry.isDiskBufferFull.test_and_set();
                    entry.isDiskBufferFull.notify_all();
                }
            }
        }

        // If an error occurred, force our worker threads to unblock and dump any data currently in the buffers.
        if (usbTransferFailed || processingFailed)
        {
            dumpAllCaptureDataInProgress.test_and_set();
            for (size_t i = 0; i < totalDiskBufferEntryCount; ++i)
            {
                DiskBufferEntry& entry = diskBufferEntries[i];
                entry.isDiskBufferFull.clear();
                entry.isDiskBufferFull.notify_all();
            }
            for (size_t i = 0; i < totalDiskBufferEntryCount; ++i)
            {
                DiskBufferEntry& entry = diskBufferEntries[i];
                entry.isDiskBufferFull.test_and_set();
                entry.isDiskBufferFull.notify_all();
            }
        }

        // Yield the remainder of the timeslice, to help keep CPU resources free while we try and spin down.
#ifdef _WIN32
        Sleep(0);
#else
        sched_yield();
#endif
    }

    // Wait for our spawned threads to terminate
    usbTransferThread.join();
    processingThread.join();

    // Set the result of this transfer process
    if (!errorCodeLatched)
    {
        result = TransferResult::Success;
    }
    captureResult = result;

    // Restore the original process priority settings
    if (boostedProcessPriority)
    {
        RestoreCurrentProcessPriority(processPriorityRestoreInfo);
    }

    // Release all our memory buffer locks
    for (size_t i = 0; i < conversionBufferCount; ++i)
    {
        UnlockMemoryBuffer(conversionBuffers[i].data(), conversionBuffers[i].size());
    }
    for (size_t i = 0; i < totalDiskBufferEntryCount; ++i)
    {
        DiskBufferEntry& entry = diskBufferEntries[i];
        UnlockMemoryBuffer(entry.readBuffer.data(), entry.readBuffer.size());
    }
    UnlockMemoryBuffer(diskBufferEntries.get(), sizeof(diskBufferEntries[0]) * totalDiskBufferEntryCount);
    UnlockMemoryBuffer(this, sizeof(*this));

    // Flag that this thread is no longer running
    captureThreadRunning.clear();
    captureThreadRunning.notify_all();
  }
  catch (const std::exception& e)
  {
      Log().Error("CaptureThread(): Unhandled exception: {0}", e.what());
      SetUsbTransferFinished(TransferResult::ProgramError);
      captureThreadRunning.clear();
      captureThreadRunning.notify_all();
  }
  catch (...)
  {
      Log().Error("CaptureThread(): Unknown unhandled exception");
      SetUsbTransferFinished(TransferResult::ProgramError);
      captureThreadRunning.clear();
      captureThreadRunning.notify_all();
  }
}

//----------------------------------------------------------------------------------------------------------------------
bool UsbDeviceBase::GetTransferInProgress() const
{
    return transferInProgress && (captureResult == TransferResult::Running);
}

//----------------------------------------------------------------------------------------------------------------------
bool UsbDeviceBase::GetCaptureSessionOpen() const
{
    return transferInProgress;
}

//----------------------------------------------------------------------------------------------------------------------
UsbDeviceBase::TransferResult UsbDeviceBase::GetTransferResult() const
{
    return captureResult;
}

//----------------------------------------------------------------------------------------------------------------------
size_t UsbDeviceBase::GetNumberOfTransfers() const
{
    return transferCount;
}

//----------------------------------------------------------------------------------------------------------------------
size_t UsbDeviceBase::GetNumberOfDiskBuffersWritten() const
{
    return transferBufferWrittenCount;
}

//----------------------------------------------------------------------------------------------------------------------
size_t UsbDeviceBase::GetFileSizeWrittenInBytes() const
{
    if (captureFormat == CaptureFormat::Signed16BitFlacOnTheFly && !captureFilePath.empty())
    {
        std::error_code error;
        const uintmax_t size = std::filesystem::file_size(captureFilePath, error);
        if (!error)
        {
            return static_cast<size_t>(size);
        }
    }
    return transferFileSizeWrittenInBytes;
}

//----------------------------------------------------------------------------------------------------------------------
size_t UsbDeviceBase::GetMinSampleValue() const
{
    return minSampleValue;
}

//----------------------------------------------------------------------------------------------------------------------
size_t UsbDeviceBase::GetMaxSampleValue() const
{
    return maxSampleValue;
}

//----------------------------------------------------------------------------------------------------------------------
size_t UsbDeviceBase::GetClippedMinSampleCount() const
{
    return clippedMinSampleCount;
}

//----------------------------------------------------------------------------------------------------------------------
size_t UsbDeviceBase::GetClippedMaxSampleCount() const
{
    return clippedMaxSampleCount;
}

//----------------------------------------------------------------------------------------------------------------------
bool UsbDeviceBase::GetTransferHadSequenceNumbers() const
{
    return (sequenceState != SequenceState::Disabled);
}

//----------------------------------------------------------------------------------------------------------------------
uint32_t UsbDeviceBase::GetCaptureSampleRateInHz() const
{
    return currentCaptureSampleRateInHz;
}

//----------------------------------------------------------------------------------------------------------------------
uint8_t UsbDeviceBase::GetHardwareDecimationFactor() const
{
    return currentHardwareDecimationFactor;
}

//----------------------------------------------------------------------------------------------------------------------
uint8_t UsbDeviceBase::GetCaptureFrontEndGainSwitches() const
{
    return captureFrontEndGainSwitches;
}

//----------------------------------------------------------------------------------------------------------------------
const std::string& UsbDeviceBase::GetGatewareVersion() const
{
    return configuredGatewareVersion;
}

//----------------------------------------------------------------------------------------------------------------------
bool UsbDeviceBase::UsbTransferDumpBuffers() const
{
    return dumpAllCaptureDataInProgress.test();
}

//----------------------------------------------------------------------------------------------------------------------
bool UsbDeviceBase::UsbTransferStopRequested() const
{
    return usbTransferStopRequested.test();
}

//----------------------------------------------------------------------------------------------------------------------
UsbDeviceBase::DiskBufferEntry& UsbDeviceBase::GetDiskBuffer(size_t bufferNo)
{
    assert(bufferNo < totalDiskBufferEntryCount);
    return diskBufferEntries[bufferNo];
}

//----------------------------------------------------------------------------------------------------------------------
size_t UsbDeviceBase::GetDiskBufferCount() const
{
    return totalDiskBufferEntryCount;
}

//----------------------------------------------------------------------------------------------------------------------
size_t UsbDeviceBase::GetSingleDiskBufferSizeInBytes() const
{
    return diskBufferSizeInBytes;
}

//----------------------------------------------------------------------------------------------------------------------
size_t UsbDeviceBase::GetUsbTransferQueueSizeInBytes() const
{
    return currentUsbTransferQueueSizeInBytes;
}

//----------------------------------------------------------------------------------------------------------------------
bool UsbDeviceBase::GetUseSmallUsbTransfers() const
{
    return currentUseSmallUsbTransfers;
}

//----------------------------------------------------------------------------------------------------------------------
void UsbDeviceBase::SetUsbTransferFinished(TransferResult result)
{
    usbTransferResult = result;
    usbTransferRunning.clear();
    usbTransferRunning.notify_all();
    captureThreadStopRequested.test_and_set();
    captureThreadStopRequested.notify_all();
}

//----------------------------------------------------------------------------------------------------------------------
void UsbDeviceBase::SetProcessingFinished(TransferResult result)
{
    processingResult = result;
    processingRunning.clear();
    processingRunning.notify_all();
    captureThreadStopRequested.test_and_set();
    captureThreadStopRequested.notify_all();
}

//----------------------------------------------------------------------------------------------------------------------
void UsbDeviceBase::AddCompletedTransferCount(size_t incrementCount)
{
    transferCount += incrementCount;
}

//----------------------------------------------------------------------------------------------------------------------
// Processing methods
//----------------------------------------------------------------------------------------------------------------------
void UsbDeviceBase::ProcessingThread()
{
  try
  {
#ifndef _WIN32
    // A FLAC child that exits closes the pipe reader. Block SIGPIPE in this
    // worker so fwrite returns EPIPE/short-write and the capture reports a
    // recoverable FileWriteError instead of terminating the whole application.
    sigset_t blockedSignals;
    sigemptyset(&blockedSignals);
    sigaddset(&blockedSignals, SIGPIPE);
    if (pthread_sigmask(SIG_BLOCK, &blockedSignals, nullptr) != 0)
    {
        Log().Warning("ProcessingThread(): Could not block SIGPIPE");
    }
#endif
    ThreadPriorityRestoreInfo priorityRestoreInfo = {};
    bool boostedThreadPriority = SetCurrentThreadRealtimePriority(priorityRestoreInfo);
    std::shared_ptr<void> currentThreadPriorityReducer;
    if (!boostedThreadPriority)
    {
        Log().Warning("ProcessingThread(): Failed to boost thread priority");
    }
    else
    {
        currentThreadPriorityReducer.reset((void*)nullptr, [&](void*) { RestoreCurrentThreadPriority(priorityRestoreInfo); });
    }

    bool transferComplete = false;
    bool processingFailure = false;
    size_t currentDiskBuffer = 0;
    while (!processingFailure && !transferComplete)
    {
        // If processing has been requested to stop, and we've reached the end of the buffered data, or we're being
        // stopped forcefully, break out of the processing loop. Note that if the stop isn't forceful, we let the
        // current loop iteration complete to allow our current pending write to be "flushed" in the case of overlapped
        // disk IO.
        DiskBufferEntry& bufferEntry = diskBufferEntries[currentDiskBuffer];
        bool flushOnly = false;
        if (processingStopRequested.test())
        {
            if (dumpAllCaptureDataInProgress.test())
            {
                transferComplete = true;
                continue;
            }
            if (!bufferEntry.isDiskBufferFull.test() || bufferEntry.dumpingBuffer.test())
            {
                flushOnly = true;
                transferComplete = true;
            }
        }

        // If this isn't a flush-only pass, retrieve, validate, process, and write the next block of data to the output
        // file.
        if (!flushOnly)
        {
            // Wait for the next disk buffer to be filled
            bufferEntry.isDiskBufferFull.wait(false);

            // If the buffer isn't really filled, we've just been woken because the buffer is being dumped while
            // stopping the capture process, loop around again. We expect processing is either being gracefully or
            // forcefully halted. The next loop iteration will allow us to determine what the case is and whether we
            // need to flush pending writes or abort the transfers in flight.
            if (bufferEntry.dumpingBuffer.test())
            {
                continue;
            }

            // Verify and strip the sequence markers from the sample data, and update our sample metrics.
            uint16_t minValue = std::numeric_limits<uint16_t>::max();
            uint16_t maxValue = std::numeric_limits<uint16_t>::min();
            size_t minClippedCount = 0;
            size_t maxClippedCount = 0;
            if (!ProcessSequenceMarkersAndUpdateSampleMetrics(currentDiskBuffer, minValue, maxValue, minClippedCount, maxClippedCount))
            {
                SetProcessingFinished(TransferResult::SequenceMismatch);
                processingFailure = true;
                continue;
            }
            minSampleValue = std::min(minSampleValue.load(), minValue);
            maxSampleValue = std::max(maxSampleValue.load(), maxValue);
            clippedMinSampleCount += minClippedCount;
            clippedMaxSampleCount += maxClippedCount;

            // If a buffer sample has been requested, capture it now.
            if (bufferSampleRequestPending.test() && (bufferSamplingRequestedLengthInBytes <= diskBufferSizeInBytes))
            {
                capturedBufferSample.assign(bufferEntry.readBuffer.data(), bufferEntry.readBuffer.data() + bufferSamplingRequestedLengthInBytes);
                bufferSampleRequestPending.clear();
                bufferSampleAvailable.test_and_set();
                bufferSampleAvailable.notify_all();
            }

            // Verify the test data sequence if required
            if (captureIsTestMode && !VerifyTestSequence(currentDiskBuffer))
            {
                SetProcessingFinished(TransferResult::VerificationError);
                processingFailure = true;
                continue;
            }

            // Convert the sample data into the requested data format
            auto& currentConversionBuffer = conversionBuffers[conversionBufferIndex];
            if (!ConvertRawSampleData(currentDiskBuffer, captureFormat, currentConversionBuffer))
            {
                SetProcessingFinished(TransferResult::ProgramError);
                processingFailure = true;
                continue;
            }

            // Write the converted data to the pipe or output file
            if (captureFormat == CaptureFormat::Signed16BitFlacOnTheFly)
            {
#ifdef _WIN32
                // Write native signed-16 little-endian data directly to flac using WriteFile.
                // Avoids fwrite/MinGW CRT which can call abort() on a broken pipe.
                if (flacStdinWriteHandle == INVALID_HANDLE_VALUE)
                {
                    Log().Error("ProcessingThread(): flacStdinWriteHandle is invalid — pipe was not opened successfully");
                    SetProcessingFinished(TransferResult::FileWriteError);
                    processingFailure = true;
                    continue;
                }
                DWORD written = 0;
                BOOL ok = WriteFile(flacStdinWriteHandle, currentConversionBuffer.data(),
                                    static_cast<DWORD>(currentConversionBuffer.size()), &written, NULL);
                if (!ok)
                {
                    Log().Error("ProcessingThread(): Failed to write to FLAC pipe (WriteFile error {0})", GetLastError());
                    SetProcessingFinished(TransferResult::FileWriteError);
                    processingFailure = true;
                    continue;
                }
                if (written != static_cast<DWORD>(currentConversionBuffer.size()))
                {
                    Log().Error("ProcessingThread(): Short write to FLAC pipe ({0} of {1} bytes)",
                        written, currentConversionBuffer.size());
                    SetProcessingFinished(TransferResult::FileWriteError);
                    processingFailure = true;
                    continue;
                }
#else
                size_t written = fwrite(currentConversionBuffer.data(), 1, currentConversionBuffer.size(), flacPipeHandle);
                if (written != currentConversionBuffer.size())
                {
                    Log().Error("ProcessingThread(): Failed to write to FLAC pipe");
                    SetProcessingFinished(TransferResult::FileWriteError);
                    processingFailure = true;
                    continue;
                }
#endif
                bufferEntry.isDiskBufferFull.clear();
                bufferEntry.isDiskBufferFull.notify_all();
                ++transferBufferWrittenCount;
                // flac owns and finalises the output file on every platform. Polling once per
                // large disk buffer keeps the GUI counter current without touching the stream.
                {
                    std::error_code ec;
                    auto sz = std::filesystem::file_size(captureFilePath, ec);
                    if (!ec) transferFileSizeWrittenInBytes = sz;
                }
            }
            else
            {
#ifdef _WIN32
            if (useWindowsOverlappedFileIo)
            {
                // Append to the end of the file using overlapped file IO. The request is queued here, not completed.
                bufferEntry.diskWriteOverlappedBuffer.Offset = 0xFFFFFFFF;
                bufferEntry.diskWriteOverlappedBuffer.OffsetHigh = 0xFFFFFFFF;
                BOOL writeFileReturn = WriteFile(windowsCaptureOutputFileHandle, currentConversionBuffer.data(), (DWORD)currentConversionBuffer.size(), NULL, &bufferEntry.diskWriteOverlappedBuffer);
                DWORD lastError = GetLastError();
                if ((writeFileReturn != 0) || (lastError != ERROR_IO_PENDING))
                {
                    Log().Error("WriteFile returned {0} with error code {1}.", writeFileReturn, lastError);
                    SetProcessingFinished(TransferResult::FileWriteError);
                    processingFailure = true;
                    continue;
                }
                bufferEntry.diskWriteInProgress = true;
            }
            else
            {
#endif
#ifdef __APPLE__
                // Check whether the write thread has hit an error
                if (macosWriteError.load())
                {
                    SetProcessingFinished(TransferResult::FileWriteError);
                    processingFailure = true;
                    continue;
                }

                // Append the converted data to the fill buffer and release the disk buffer immediately
                // so USB transfers can continue while the write thread drains the queue.
                size_t appendSize = currentConversionBuffer.size();
                macosWriteFillBuffer.insert(macosWriteFillBuffer.end(),
                    currentConversionBuffer.begin(), currentConversionBuffer.end());
                bufferEntry.isDiskBufferFull.clear();
                bufferEntry.isDiskBufferFull.notify_all();
                ++transferBufferWrittenCount;
                transferFileSizeWrittenInBytes += appendSize;

                // When we've accumulated a full chunk, hand it off to the write thread
                if (macosWriteFillBuffer.size() >= macosWriteChunkBytes)
                {
                    std::unique_lock<std::mutex> lock(macosWriteMutex);
                    macosWriteCv.wait(lock, [this]{
                        return macosWriteQueue.size() < macosWriteQueueMaxChunks ||
                               macosWriteError.load() ||
                               processingStopRequested.test();
                    });
                    if (!macosWriteError.load() && !processingStopRequested.test())
                    {
                        macosWriteQueue.push_back(std::move(macosWriteFillBuffer));
                        macosWriteFillBuffer.clear();
                        macosWriteFillBuffer.reserve(macosWriteChunkBytes);
                    }
                    lock.unlock();
                    macosWriteCv.notify_all();
                }
#else
                // Perform the file write in a blocking operation
                captureOutputFile.write((const char*)currentConversionBuffer.data(), currentConversionBuffer.size());
                if (!captureOutputFile.good())
                {
                    Log().Error("ProcessingThread(): An error occurred when writing to the output file");
                    SetProcessingFinished(TransferResult::FileWriteError);
                    processingFailure = true;
                    continue;
                }

                // Mark the disk buffer as empty, notifying the USB transfer thread in case it's blocking waiting for this
                // buffer to be free.
                bufferEntry.isDiskBufferFull.clear();
                bufferEntry.isDiskBufferFull.notify_all();

                // Add the totals from this buffer to the transfer statistics
                ++transferBufferWrittenCount;
                transferFileSizeWrittenInBytes += currentConversionBuffer.size();
#endif
#ifdef _WIN32
            }
#endif
            } // end else (not FlacOnTheFly)
        }

        // If we're using overlapped file IO, complete the previously submitted write operation.
#ifdef _WIN32
        if (useWindowsOverlappedFileIo && captureFormat != CaptureFormat::Signed16BitFlacOnTheFly)
        {
            // Retrive the previous disk buffer entry
            size_t lastBufferIndex = (currentDiskBuffer + (totalDiskBufferEntryCount - 1)) % totalDiskBufferEntryCount;
            size_t lastConversionBufferIndex = (conversionBufferIndex + (conversionBufferCount - 1)) % conversionBufferCount;
            DiskBufferEntry& lastBufferEntry = diskBufferEntries[lastBufferIndex];

            // If the previous disk buffer entry had an overlapped write in progress, block waiting for it to complete
            // and check the result.
            if (lastBufferEntry.diskWriteInProgress)
            {
                // Block to check the result of the previous disk write operation
                DWORD bytesTransferred = 0;
                BOOL getOverlappedResultReturn = GetOverlappedResult(windowsCaptureOutputFileHandle, &lastBufferEntry.diskWriteOverlappedBuffer, &bytesTransferred, TRUE);
                if (getOverlappedResultReturn == 0)
                {
                    DWORD lastError = GetLastError();
                    Log().Error("GetOverlappedResult failed with error code {0}.", lastError);
                    SetProcessingFinished(TransferResult::FileWriteError);
                    processingFailure = true;
                    continue;
                }

                // Ensure that the correct number of bytes were written. This should always be the case if it succeeded,
                // but we check anyway.
                auto& lastConversionBuffer = conversionBuffers[lastConversionBufferIndex];
                if (bytesTransferred != lastConversionBuffer.size())
                {
                    Log().Error("ProcessingThread(): Expected {0} bytes written to disk but only {1} bytes were saved from buffer index {2}.", lastConversionBuffer.size(), bytesTransferred, lastBufferIndex);
                    SetProcessingFinished(TransferResult::FileWriteError);
                    processingFailure = true;
                    continue;
                }

                // Mark the previous disk buffer as empty, notifying the USB transfer thread in case it's blocking
                // waiting for this buffer to be free.
                lastBufferEntry.diskWriteInProgress = false;
                lastBufferEntry.isDiskBufferFull.clear();
                lastBufferEntry.isDiskBufferFull.notify_all();

                // Add the totals from this last buffer to the transfer statistics
                ++transferBufferWrittenCount;
                transferFileSizeWrittenInBytes += lastConversionBuffer.size();
            }
        }
#endif

        // Advance to the next disk buffer in the queue
        currentDiskBuffer = (currentDiskBuffer + 1) % totalDiskBufferEntryCount;
#ifdef _WIN32
        if (useWindowsOverlappedFileIo)
        {
            conversionBufferIndex = (conversionBufferIndex + 1) % conversionBufferCount;
        }
#endif
    }

    // If we're using overlapped file IO and a processing failure occurred, cancel any IO operations still in progress
    // on the output file.
#ifdef _WIN32
    if (useWindowsOverlappedFileIo && processingFailure)
    {
        BOOL cancelIoReturn = CancelIo(windowsCaptureOutputFileHandle);
        if (cancelIoReturn == 0)
        {
            DWORD lastError = GetLastError();
            Log().Error("CancelIo failed with error code {0}.", lastError);
        }
    }
#endif

    // If we've been requested to stop the capture process, and an error hasn't been flagged, mark the process as
    // successful.
    if (!processingFailure)
    {
        SetProcessingFinished(TransferResult::Success);
    }
  }
  catch (const std::exception& e)
  {
      Log().Error("ProcessingThread(): Unhandled exception: {0}", e.what());
      SetProcessingFinished(TransferResult::ProgramError);
  }
  catch (...)
  {
      Log().Error("ProcessingThread(): Unknown unhandled exception");
      SetProcessingFinished(TransferResult::ProgramError);
  }
}

//----------------------------------------------------------------------------------------------------------------------
// macOS write thread
//----------------------------------------------------------------------------------------------------------------------
#ifdef __APPLE__
void UsbDeviceBase::MacosWriteThread()
{
    while (true)
    {
        std::vector<uint8_t> chunk;
        {
            std::unique_lock<std::mutex> lock(macosWriteMutex);
            macosWriteCv.wait(lock, [this]{ return !macosWriteQueue.empty() || macosWriteThreadExit; });
            if (macosWriteQueue.empty())
                break; // exit requested and no more data
            chunk = std::move(macosWriteQueue.front());
            macosWriteQueue.pop_front();
        }
        // Notify the processing thread that a slot in the queue is free
        macosWriteCv.notify_all();

        const uint8_t* ptr = chunk.data();
        ssize_t remaining = static_cast<ssize_t>(chunk.size());
        while (remaining > 0)
        {
            ssize_t n = ::write(macosOutputFd, ptr, static_cast<size_t>(remaining));
            if (n < 0)
            {
                if (errno == EINTR) continue;
                Log().Error("MacosWriteThread(): write() failed with errno {0}", errno);
                macosWriteError.store(true);
                break;
            }
            ptr += n;
            remaining -= n;
        }
        if (macosWriteError.load()) break;
    }
}
#endif

//----------------------------------------------------------------------------------------------------------------------
bool UsbDeviceBase::ProcessSequenceMarkersAndUpdateSampleMetrics(size_t diskBufferIndex, uint16_t& minValue, uint16_t& maxValue, size_t& minClippedCount, size_t& maxClippedCount)
{
    // If sequence checking has already failed, return false immediately. This condition should not occur, as a
    // sequence failure is treated as an unrecoverable error and aborts the capture process. If this is to be changed,
    // note that we would still need to perform sequence number stripping and update our sample metrics below, so this
    // condition here should be removed and the rest of the function allowed to run.
    if (sequenceState == SequenceState::Failed)
    {
        return false;
    }

    // Retrieve the previous sequence counter number. If we don't have a saved sequence number because we're just
    // starting a capture and we need to synchronize with the device, extract the sequence number from the buffer
    // and calculate the previous sequence number from it as a starting point.
    const uint16_t minPossibleSampleValue = 0;
    const uint16_t maxPossibleSampleValue = 0b1111111111;
    const int COUNTER_SHIFT = 16;
    const uint32_t COUNTER_MAX = 0b111111;
    uint32_t sequenceCounter = savedSequenceCounter;
    uint8_t* diskBuffer = diskBufferEntries[diskBufferIndex].readBuffer.data();
    if (sequenceState == SequenceState::Sync)
    {
        // Initialize the sequence counter to an impossible value
        sequenceCounter = 0xFFFFFFFF;

        // Get the first sequence number from the buffer
        uint32_t firstSequenceNumber = (uint32_t)(diskBuffer[1] >> 2);

        // Find the first time the sequence number changes. Since each sequence number appears on (1 << COUNTER_SHIFT)
        // samples, at worst we will see a change within (1 << COUNTER_SHIFT) + 1 samples.
        for (size_t pointer = 2; pointer < ((1 << COUNTER_SHIFT) + 1) * 2; pointer += 2)
        {
            uint32_t sequenceNumber = diskBuffer[pointer + 1] >> 2;
            if (sequenceNumber != firstSequenceNumber)
            {
                // Found it -- compute sequenceCounter's value at the start of the buffer
                if (sequenceNumber == 0)
                {
                    sequenceNumber = COUNTER_MAX;
                }
                sequenceCounter = (sequenceNumber << COUNTER_SHIFT) - ((uint32_t)pointer / 2);
                break;
            }
        }

        // If no sequence numbers were detected, disable sequence checking, otherwise activate it.
        if (sequenceCounter == 0xFFFFFFFF)
        {
            Log().Warning("ProcessSequenceMarkersAndUpdateSampleMetrics(): Data does not include sequence numbers");
            sequenceState = SequenceState::Disabled;
        }
        else
        {
            Log().Trace("ProcessSequenceMarkersAndUpdateSampleMetrics(): Synchronised with data sequence numbers");
            sequenceState = SequenceState::Running;
        }
    }

    // Validate sequence number progression, and update our sample metrics.
    for (size_t i = 0; i < diskBufferSizeInBytes; i += 2)
    {
        // If sequence checking is active, validate sequence numbers for this sample.
        if (sequenceState == SequenceState::Running)
        {
            // Ensure the sequence number in this sample matches the expected value. Note that this is treated as an
            // unrecoverable and immediate fail, so we abort any further processing and return false here.
            uint32_t expected = sequenceCounter >> COUNTER_SHIFT;
            uint32_t sequenceNumber = (uint32_t)(diskBuffer[i + 1] >> 2);
            if (sequenceNumber != expected)
            {
                Log().Error("ProcessSequenceMarkersAndUpdateSampleMetrics(): Sequence number mismatch! Expecting {0} but got {1}", expected, sequenceNumber);
                sequenceState = SequenceState::Failed;
                savedSequenceCounter = sequenceCounter;
                return false;
            }

            // Advance the sequence counter
            ++sequenceCounter;
            if (sequenceCounter == (COUNTER_MAX << COUNTER_SHIFT))
            {
                sequenceCounter = 0;
            }
        }

        // Remove the sequence number from the sample in the disk buffer (modify the source data)
        diskBuffer[i + 1] &= 0x03;

        // Get the original 10-bit unsigned value from the disk data buffer
        uint16_t originalValue = (uint16_t)diskBuffer[i + 0] | ((uint16_t)diskBuffer[i + 1] << 8);

        // Update our min/max values
        minValue = std::min(minValue, originalValue);
        maxValue = std::max(maxValue, originalValue);

        // If the sample value is either the minimum or maximum value, increment our clipped sample counts.
        if (originalValue == minPossibleSampleValue)
        {
            ++minClippedCount;
        }
        else if (originalValue == maxPossibleSampleValue)
        {
            ++maxClippedCount;
        }
    }

    // Save the resulting sequence counter so we can continue checking from the same position in the next buffer
    savedSequenceCounter = sequenceCounter;
    return true;
}

//----------------------------------------------------------------------------------------------------------------------
bool UsbDeviceBase::VerifyTestSequence(size_t diskBufferIndex)
{
    // Retrieve the stored expected next sample value. If we haven't processed a buffer in this capture yet,
    // latch the first sample value as the expected value so we can start from here.
    DiskBufferEntry& bufferEntry = diskBufferEntries[diskBufferIndex];
    uint16_t expectedValue = expectedNextTestDataValue.value_or((uint16_t)bufferEntry.readBuffer[0] | (uint16_t)((uint16_t)bufferEntry.readBuffer[1] << 8));

    // Verify each sample in the buffer matches our expected sequence progression
    const uint8_t* readBufferPointer = bufferEntry.readBuffer.data();
    for (size_t i = 0; i < bufferEntry.readBuffer.size(); i += 2)
    {
        // Get the original 10-bit unsigned value from the disk data buffer
        uint16_t actualValue = (uint16_t)readBufferPointer[0] | ((uint16_t)readBufferPointer[1] << 8);
        readBufferPointer += 2;

        // If the actual value doesn't match our expected value, but this is the first time the test sequence
        // has wrapped around to 0, check if this appears to be the wrap point for the sequence, and latch it.
        // Valid wrapp points are either 1021 (newer FPGA firmware) or 1024 (older FPGA firmware).
        if (!testDataMax.has_value() && (expectedValue != actualValue) && (actualValue == 0) && ((expectedValue == 1021) || (expectedValue == 1024)))
        {
            testDataMax = expectedValue;
            expectedValue = 1;
            continue;
        }

        // If the expected value differs from the actual value, log an error.
        if (expectedValue != actualValue)
        {
            // Data error
            Log().Error("VerifyTestSequence(): Data error in test data verification! Expecting {0} but got {1}", expectedValue, actualValue);
            return false;
        }

        // Calculate the value we expect to find for the next sample
        ++expectedValue;
        if (testDataMax.has_value() && (expectedValue == testDataMax))
        {
            expectedValue = 0;
        }
    }

    // Store the next expected sample value so we can check against the next buffer
    expectedNextTestDataValue = expectedValue;
    return true;
}

//----------------------------------------------------------------------------------------------------------------------
bool UsbDeviceBase::ConvertRawSampleData(size_t diskBufferIndex, CaptureFormat captureFormat, std::vector<uint8_t>& outputBuffer) const
{
    const DiskBufferEntry& bufferEntry = diskBufferEntries[diskBufferIndex];
    const uint8_t* readBufferPointer = bufferEntry.readBuffer.data();
    size_t readBufferSizeInBytes = bufferEntry.readBuffer.size();

    // Convert the data to the required format
    uint8_t* writeBufferPointer = outputBuffer.data();
    if (captureFormat == CaptureFormat::Signed16Bit
        || captureFormat == CaptureFormat::Signed16BitFlacOnTheFly)
    {
        // Translate the data in the disk buffer to scaled 16-bit signed data
        // For FlacOnTheFly this exact stream is fed directly to the native FLAC encoder.
        for (size_t i = 0; i < readBufferSizeInBytes; i += 2)
        {
            // Get the original 10-bit unsigned value from the disk data buffer
            uint16_t originalValue = (uint16_t)readBufferPointer[0] | ((uint16_t)readBufferPointer[1] << 8);
            readBufferPointer += 2;

            // Sign and scale the data to 16-bits. Technically a line like this would use the entire 16-bit range:
            //uint16_t signedValue = ((uint16_t)((int16_t)originalValue - 0x0200) << 6) | ((originalValue >> 4) & 0x003F);
            // In our case here however, that would not be preferred, since we can't restore the lost 6 bits of
            // precision, and where we guess wrong we'd create very slight frequency distortions. It's better to leave
            // the data as 10-bit and just shift it up, which doesn't technically preserve the relative mplitude of the
            // signal, but we don't care about the overall amplitude in this case, it's the frequency we care about.
            uint16_t signedValue = (uint16_t)((int16_t)originalValue - 0x0200) << 6;
            writeBufferPointer[0] = (uint8_t)((uint16_t)signedValue & 0x00FF);
            writeBufferPointer[1] = (uint8_t)(((uint16_t)signedValue & 0xFF00) >> 8);
            writeBufferPointer += 2;
        }
    }
    else if (captureFormat == CaptureFormat::Signed16BitHalf || captureFormat == CaptureFormat::Signed16BitQuarter)
    {
        // First convert to 16-bit signed data, then downsample
        size_t sampleCount = readBufferSizeInBytes / 2;
        
        // Ensure our input buffer is large enough
        resampleInputBuffer.resize(sampleCount);
        
        // Convert 10-bit to 16-bit signed samples
        const uint8_t* srcPtr = readBufferPointer;
        for (size_t i = 0; i < sampleCount; ++i)
        {
            uint16_t originalValue = (uint16_t)srcPtr[0] | ((uint16_t)srcPtr[1] << 8);
            srcPtr += 2;
            
            // Convert to signed 16-bit (same logic as above)
            int16_t signedValue = ((int16_t)originalValue - 0x0200) << 6;
            resampleInputBuffer[i] = signedValue;
        }
        
        // Determine downsampling factor and initialize resampler if needed
        int downsampleFactor = (captureFormat == CaptureFormat::Signed16BitHalf) ? 2 : 4;
        uint32_t inputSampleRate = 40000000;  // 40 MSPS
        uint32_t outputSampleRate = inputSampleRate / downsampleFactor;

        // Reinitialize if not yet initialized or if rate changed (e.g. second capture at a different rate)
        if (!audioResampler.isInitialized() || audioResampler.getOutputSampleRate() != outputSampleRate)
        {
            audioResampler.cleanup();
            if (!audioResampler.initialize(inputSampleRate, outputSampleRate))
            {
                Log().Error("ConvertRawSampleData(): Failed to initialize audio resampler for format {0}", (int)captureFormat);
                return false;
            }
        }
        
        // Calculate expected output size
        int expectedOutputSamples = audioResampler.getExpectedOutputSampleCount((int)sampleCount);
        resampleOutputBuffer.resize(expectedOutputSamples);
        
        // Perform resampling
        int actualOutputSamples = audioResampler.resample(resampleInputBuffer.data(), (int)sampleCount, 
                                                         resampleOutputBuffer.data(), expectedOutputSamples);
        if (actualOutputSamples < 0)
        {
            Log().Error("ConvertRawSampleData(): Failed to resample audio data for format {0}", (int)captureFormat);
            return false;
        }
        
        // Copy resampled data to output buffer as bytes
        size_t outputSizeInBytes = (size_t)actualOutputSamples * 2;  // 2 bytes per 16-bit sample
        const int16_t* resampledData = resampleOutputBuffer.data();
        
        for (int i = 0; i < actualOutputSamples; ++i)
        {
            int16_t sample = resampledData[i];
            writeBufferPointer[0] = (uint8_t)(sample & 0x00FF);
            writeBufferPointer[1] = (uint8_t)((sample & 0xFF00) >> 8);
            writeBufferPointer += 2;
        }
        
        // Adjust the output buffer size to reflect actual output
        outputBuffer.resize(outputSizeInBytes);
    }
    else if (captureFormat == CaptureFormat::Unsigned10Bit)
    {
        // Translate the data in the disk buffer to unsigned 10-bit packed data
        for (size_t i = 0; i < readBufferSizeInBytes; i += 8)
        {
            // Get the original 4 10-bit words
            uint16_t originalWords[4];
            originalWords[0] = (uint16_t)readBufferPointer[0] | ((uint16_t)readBufferPointer[1] << 8);
            originalWords[1] = (uint16_t)readBufferPointer[2] | ((uint16_t)readBufferPointer[3] << 8);
            originalWords[2] = (uint16_t)readBufferPointer[4] | ((uint16_t)readBufferPointer[5] << 8);
            originalWords[3] = (uint16_t)readBufferPointer[6] | ((uint16_t)readBufferPointer[7] << 8);
            readBufferPointer += 8;

            // Convert into 5 bytes of packed 10-bit data
            writeBufferPointer[0] = (uint8_t)((originalWords[0] & 0x03FC) >> 2);
            writeBufferPointer[1] = (uint8_t)((originalWords[0] & 0x0003) << 6) | (uint8_t)((originalWords[1] & 0x03F0) >> 4);
            writeBufferPointer[2] = (uint8_t)((originalWords[1] & 0x000F) << 4) | (uint8_t)((originalWords[2] & 0x03C0) >> 6);
            writeBufferPointer[3] = (uint8_t)((originalWords[2] & 0x003F) << 2) | (uint8_t)((originalWords[3] & 0x0300) >> 8);
            writeBufferPointer[4] = (uint8_t)((originalWords[3] & 0x00FF));
            writeBufferPointer += 5;
        }
    }
    else if (captureFormat == CaptureFormat::Unsigned10Bit4to1Decimation)
    {
        // Translate the data in the disk buffer to unsigned 10-bit packed data with 4:1 decimation
        for (size_t i = 0; i < readBufferSizeInBytes; i += (8 * 4))
        {
            // Get the original 4 10-bit words
            uint16_t originalWords[4];
            originalWords[0] = (uint16_t)readBufferPointer[0 + 0] | ((uint16_t)readBufferPointer[1 + 0] << 8);
            originalWords[1] = (uint16_t)readBufferPointer[2 + 4] | ((uint16_t)readBufferPointer[3 + 4] << 8);
            originalWords[2] = (uint16_t)readBufferPointer[4 + 8] | ((uint16_t)readBufferPointer[5 + 8] << 8);
            originalWords[3] = (uint16_t)readBufferPointer[6 + 12] | ((uint16_t)readBufferPointer[7 + 12] << 8);
            readBufferPointer += 8 * 4;

            // Convert into 5 bytes of packed 10-bit data
            writeBufferPointer[0] = (uint8_t)((originalWords[0] & 0x03FC) >> 2);
            writeBufferPointer[1] = (uint8_t)((originalWords[0] & 0x0003) << 6) | (uint8_t)((originalWords[1] & 0x03F0) >> 4);
            writeBufferPointer[2] = (uint8_t)((originalWords[1] & 0x000F) << 4) | (uint8_t)((originalWords[2] & 0x03C0) >> 6);
            writeBufferPointer[3] = (uint8_t)((originalWords[2] & 0x003F) << 2) | (uint8_t)((originalWords[3] & 0x0300) >> 8);
            writeBufferPointer[4] = (uint8_t)((originalWords[3] & 0x00FF));
            writeBufferPointer += 5;
        }
    }
    else
    {
        Log().Error("ConvertRawSampleData(): Unknown capture format {0} specified", captureFormat);
        return false;
    }
    return true;
}

//----------------------------------------------------------------------------------------------------------------------
// Buffer sampling methods
//----------------------------------------------------------------------------------------------------------------------
void UsbDeviceBase::QueueBufferSampleRequest(size_t requestedSampleLengthInBytes)
{
    bufferSamplingRequestedLengthInBytes = requestedSampleLengthInBytes;
    bufferSampleRequestPending.test_and_set();
}

//----------------------------------------------------------------------------------------------------------------------
bool UsbDeviceBase::GetNextBufferSample(std::vector<uint8_t>& bufferSample)
{
    // If no new buffer sample is available, abort any further processing.
    if (!bufferSampleAvailable.test())
    {
        return false;
    }

    // Copy the requested data into the buffer
    bufferSampleAvailable.clear();
    bufferSample.assign(capturedBufferSample.data(), capturedBufferSample.data() + capturedBufferSample.size());
    return true;
}

//----------------------------------------------------------------------------------------------------------------------
// Utility methods
//----------------------------------------------------------------------------------------------------------------------
bool UsbDeviceBase::LockMemoryBufferIntoPhysicalMemory(void* baseAddress, size_t sizeInBytes)
{
#ifdef _WIN32
    // Retrieve the Windows page allocation size
    SYSTEM_INFO sysInfo;
    GetSystemInfo(&sysInfo);
    size_t systemPageSizeInBytes = (size_t)sysInfo.dwPageSize;

    // Increase the process working set size on Windows, to allow for the extra allocation required by the locked
    // buffers. If we don't do this, VirtualLock will fail if we exceed the initial allocation. Note that since memory
    // allocations may straddle page boundaries, we pad out an extra two memory pages per locked region.
    size_t workingSetSizeIncreaseOverBaseline = (lockedMemorySizeInBytes + sizeInBytes) + (systemPageSizeInBytes * 2 * (lockedMemoryBufferCount + 1));
    size_t newMinWorkingSetSize = originalProcessMinimumWorkingSetSizeInBytes + workingSetSizeIncreaseOverBaseline;
    size_t newMaxWorkingSetSize = originalProcessMaximumWorkingSetSizeInBytes + workingSetSizeIncreaseOverBaseline;
    BOOL setProcessWorkingSetSizeReturn = SetProcessWorkingSetSize(GetCurrentProcess(), newMinWorkingSetSize, newMaxWorkingSetSize);
    if (setProcessWorkingSetSizeReturn == 0)
    {
        DWORD lastError = GetLastError();
        Log().Error("SetProcessWorkingSetSize failed with error code {0}", lastError);
        return false;
    }
#endif

    // Lock the target memory buffer into physical memory
#ifdef _WIN32
    BOOL virtualLockReturn = VirtualLock(baseAddress, sizeInBytes);
    if (virtualLockReturn == 0)
    {
        DWORD lastError = GetLastError();
        Log().Error("VirtualLock failed with error code {0}", lastError);
        return false;
    }
#else
    if (mlock(baseAddress, sizeInBytes) == -1)
    {
        Log().Error("mlock failed");
        return false;
    }
#endif

    // Increase the totals of locked memory
    lockedMemorySizeInBytes += sizeInBytes;
    ++lockedMemoryBufferCount;
    return true;
}

//----------------------------------------------------------------------------------------------------------------------
void UsbDeviceBase::UnlockMemoryBuffer(void* baseAddress, size_t sizeInBytes)
{
    // Release the lock on the target memory buffer
#ifdef _WIN32
    BOOL virtualUnlockReturn = VirtualUnlock(baseAddress, sizeInBytes);
    if (virtualUnlockReturn == 0)
    {
        DWORD lastError = GetLastError();
        Log().Error("VirtualUnlock failed with error code {0}", lastError);
        return;
    }
#else
    munlock(baseAddress, sizeInBytes);
#endif

    // Decrease the totals of locked memory
    lockedMemorySizeInBytes -= sizeInBytes;
    --lockedMemoryBufferCount;

#ifdef _WIN32
    // Retrieve the Windows page allocation size
    SYSTEM_INFO sysInfo;
    GetSystemInfo(&sysInfo);
    size_t systemPageSizeInBytes = (size_t)sysInfo.dwPageSize;

    // Reduce the process working set size on Windows
    size_t workingSetSizeIncreaseOverBaseline = (lockedMemorySizeInBytes + sizeInBytes) + (systemPageSizeInBytes * 2 * (lockedMemoryBufferCount + 1));
    size_t newMinWorkingSetSize = originalProcessMinimumWorkingSetSizeInBytes + workingSetSizeIncreaseOverBaseline;
    size_t newMaxWorkingSetSize = originalProcessMaximumWorkingSetSizeInBytes + workingSetSizeIncreaseOverBaseline;
    BOOL setProcessWorkingSetSizeReturn = SetProcessWorkingSetSize(GetCurrentProcess(), newMinWorkingSetSize, newMaxWorkingSetSize);
    if (setProcessWorkingSetSizeReturn == 0)
    {
        DWORD lastError = GetLastError();
        Log().Error("SetProcessWorkingSetSize failed with error code {0}", lastError);
        return;
    }
#endif
}

//----------------------------------------------------------------------------------------------------------------------
#ifdef _WIN32
bool UsbDeviceBase::SetCurrentProcessRealtimePriority(ProcessPriorityRestoreInfo& priorityRestoreInfo)
{
    // Request the realtime priority class on Windows. If the process doesn't have administrator rights, we may not
    // obtain realtime priority, but in that case the call will still succeed, and give us the highest priority class
    // we can obtain with the current process privileges.
    int originalPriorityClass = GetPriorityClass(GetCurrentProcess());
    int requestedPriorityClass = REALTIME_PRIORITY_CLASS;
    BOOL setPriorityClassReturn = SetPriorityClass(GetCurrentProcess(), requestedPriorityClass);
    if (setPriorityClassReturn == 0)
    {
        DWORD lastError = GetLastError();
        Log().Error("SetPriorityClass failed with error code {0}", lastError);
        return false;
    }

    // Confirm the priority class we ended up obtaining
    int newPriorityClass = GetPriorityClass(GetCurrentProcess());
    priorityRestoreInfo.originalPriorityClass = originalPriorityClass;
    Log().Info("SetCurrentProcessRealtimePriority: Requesting process priority {0} gave us {1}", requestedPriorityClass, newPriorityClass);
    return true;
}
#endif

//----------------------------------------------------------------------------------------------------------------------
#ifndef _WIN32
bool UsbDeviceBase::SetCurrentProcessRealtimePriority(ProcessPriorityRestoreInfo& priorityRestoreInfo)
{
    // Process priority is not a supported concept on a Linux-based OS
    return true;
}
#endif

//----------------------------------------------------------------------------------------------------------------------
#ifdef _WIN32
void UsbDeviceBase::RestoreCurrentProcessPriority(const ProcessPriorityRestoreInfo& priorityRestoreInfo)
{
    // Restore the original process priority class
    BOOL setPriorityClassReturn = SetPriorityClass(GetCurrentProcess(), priorityRestoreInfo.originalPriorityClass);
    if (setPriorityClassReturn == 0)
    {
        DWORD lastError = GetLastError();
        Log().Error("SetPriorityClass failed with error code {0}", lastError);
        return;
    }
}
#endif

//----------------------------------------------------------------------------------------------------------------------
#ifndef _WIN32
void UsbDeviceBase::RestoreCurrentProcessPriority(const ProcessPriorityRestoreInfo& priorityRestoreInfo)
{
    // Process priority is not a supported concept on a Linux-based OS
}
#endif

//----------------------------------------------------------------------------------------------------------------------
#ifdef _WIN32
bool UsbDeviceBase::SetCurrentThreadRealtimePriority(ThreadPriorityRestoreInfo& priorityRestoreInfo)
{
    // Retrieve the current thread priority
    HANDLE currentThreadHandle = GetCurrentThread();
    int originalThreadPriority = GetThreadPriority(currentThreadHandle);
    if (originalThreadPriority == THREAD_PRIORITY_ERROR_RETURN)
    {
        DWORD lastError = GetLastError();
        Log().Error("GetThreadPriority failed with error code {0}", lastError);
        return false;
    }

    // Attempt to increase thread priority to time critical
    int requestedPriority = THREAD_PRIORITY_TIME_CRITICAL;
    BOOL setThreadPriorityReturn = SetThreadPriority(currentThreadHandle, requestedPriority);
    if (setThreadPriorityReturn == 0)
    {
        DWORD lastError = GetLastError();
        Log().Error("SetThreadPriority failed with error code {0}", lastError);
        return false;
    }

    // Confirm the thread priority we ended up obtaining
    int newThreadPriority = GetThreadPriority(currentThreadHandle);
    priorityRestoreInfo.originalPriority = originalThreadPriority;
    Log().Info("SetCurrentThreadRealtimePriority: Requesting thread priority {0} gave us {1}", requestedPriority, newThreadPriority);
    return true;
}
#endif

//----------------------------------------------------------------------------------------------------------------------
#ifndef _WIN32
bool UsbDeviceBase::SetCurrentThreadRealtimePriority(ThreadPriorityRestoreInfo& priorityRestoreInfo)
{
    // Retrieve the current scheduling policy for the calling thread
    int oldSchedPolicy;
    sched_param oldSchedParam;
    pthread_getschedparam(pthread_self(), &oldSchedPolicy, &oldSchedParam);
    priorityRestoreInfo.oldSchedPolicy = oldSchedPolicy;
    priorityRestoreInfo.oldSchedParam = oldSchedParam;

    // Attempt to increase the thread scheduling policy to realtime
    int targetPolicy = SCHED_RR;
    int minSchedPriority = sched_get_priority_min(targetPolicy);
    int maxSchedPriority = sched_get_priority_max(targetPolicy);
    sched_param schedParams;
    if (minSchedPriority == -1 || maxSchedPriority == -1)
    {
        schedParams.sched_priority = 0;
    }
    else
    {
        // Put the priority about 3/4 of the way through its range
        schedParams.sched_priority = (minSchedPriority + (3 * maxSchedPriority)) / 4;
    }
    if (pthread_setschedparam(pthread_self(), targetPolicy, &schedParams) == 0)
    {
        Log().Info("SetCurrentThreadRealtimePriority: Thread priority set with policy SCHED_RR");
    }
    else
    {
        Log().Warning("SetCurrentThreadRealtimePriority: Unable to set thread priority");
    }
    return true;
}
#endif

//----------------------------------------------------------------------------------------------------------------------
#ifdef _WIN32
void UsbDeviceBase::RestoreCurrentThreadPriority(const ThreadPriorityRestoreInfo& priorityRestoreInfo)
{
    SetThreadPriority(GetCurrentThread(), priorityRestoreInfo.originalPriority);
}
#endif

//----------------------------------------------------------------------------------------------------------------------
#ifndef _WIN32
void UsbDeviceBase::RestoreCurrentThreadPriority(const ThreadPriorityRestoreInfo& priorityRestoreInfo)
{
    if (pthread_setschedparam(pthread_self(), priorityRestoreInfo.oldSchedPolicy, &priorityRestoreInfo.oldSchedParam) != 0)
    {
        Log().Warning("RestoreCurrentThreadPriority: Unable to restore original scheduling policy");
    }
}
#endif
