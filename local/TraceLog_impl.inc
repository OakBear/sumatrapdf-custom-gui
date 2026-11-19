/* Implementation of local/TraceLog.h. Include it from exactly one translation
   unit (SumatraPDF.cpp); every other user includes the header only. */

namespace tracelog {

namespace {

constexpr int kRingLines = 128;
constexpr int kLineLen = 512;
constexpr int kDrainBatch = 32;
constexpr ULONGLONG kStallMs = 1200;      // UI quiet this long counts as a stall
constexpr ULONGLONG kStallRepeatMs = 3000;  // log a growing stall this often
constexpr ULONGLONG kTickMs = 200;
constexpr unsigned long long kMaxFileBytes = 4ULL * 1024ULL * 1024ULL;

SRWLOCK gLock = SRWLOCK_INIT;
char gRing[kRingLines][kLineLen];
int gHead = 0;
int gTail = 0;
int gDropped = 0;

volatile LONG gHeartbeat = 0;  // low 32 bits of GetTickCount64()
const char* volatile gStage = "start";
char gDocPath[1024];
bool gDocChanged = false;

volatile LONG gStarted = 0;
volatile LONG gStop = 0;
HANDLE gWake = nullptr;
HANDLE gThread = nullptr;
HANDLE gFile = INVALID_HANDLE_VALUE;
wchar_t gFilePath[MAX_PATH * 2] = {};
unsigned long long gFileBytes = 0;
bool gWroteBom = false;

void Timestamp(char* buf, size_t n) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    snprintf(buf, n, "%02d:%02d:%02d.%03d", (int)st.wHour, (int)st.wMinute, (int)st.wSecond, (int)st.wMilliseconds);
}

const char* BaseName(const char* path) {
    if (!path) {
        return "";
    }
    const char* last = path;
    for (const char* p = path; *p; p++) {
        if (*p == '\\' || *p == '/') {
            last = p + 1;
        }
    }
    return last;
}

void PushRaw(const char* text) {
    AcquireSRWLockExclusive(&gLock);
    int next = (gHead + 1) % kRingLines;
    if (next == gTail) {
        gDropped++;
    } else {
        strncpy(gRing[gHead], text, kLineLen - 1);
        gRing[gHead][kLineLen - 1] = 0;
        gHead = next;
    }
    ReleaseSRWLockExclusive(&gLock);
    if (gWake) {
        SetEvent(gWake);
    }
}

void WriteBytes(const char* data, DWORD n) {
    if (gFile == INVALID_HANDLE_VALUE || n == 0) {
        return;
    }
    DWORD written = 0;
    if (WriteFile(gFile, data, n, &written, nullptr)) {
        gFileBytes += written;
    }
}

// Same as WriteLine but stamps at write time; for lines the worker generates
// itself (stalls), which never sat in the queue.
void WriteStampLine(const char* text) {
    char buf[kLineLen + 16];
    char ts[32];
    Timestamp(ts, sizeof(ts));
    int used = snprintf(buf, sizeof(buf), "%s %s\r\n", ts, text);
    if (used <= 0) {
        return;
    }
    if (used > (int)sizeof(buf) - 1) {
        used = (int)sizeof(buf) - 1;
    }
    WriteBytes(buf, (DWORD)used);
}

// One event line. Events are timestamped when they are queued, not when the
// worker gets around to writing them, otherwise a burst of queued lines all look
// like they happened at drain time.
void WriteLine(const char* text) {
    char buf[kLineLen + 16];
    int used = snprintf(buf, sizeof(buf), "%s\r\n", text);
    if (used < 0) {
        return;
    }
    if (used > (int)sizeof(buf) - 1) {
        used = (int)sizeof(buf) - 1;
    }
    WriteBytes(buf, (DWORD)used);
}

void CloseFile() {
    if (gFile != INVALID_HANDLE_VALUE) {
        CloseHandle(gFile);
        gFile = INVALID_HANDLE_VALUE;
    }
}

void RotateIfNeeded() {
    if (gFile == INVALID_HANDLE_VALUE || gFileBytes < kMaxFileBytes || !gFilePath[0]) {
        return;
    }
    CloseFile();
    wchar_t oldPath[MAX_PATH * 2] = {};
    swprintf_s(oldPath, MAX_PATH * 2, L"%s.1", gFilePath);
    MoveFileExW(gFilePath, oldPath, MOVEFILE_REPLACE_EXISTING);
    gFileBytes = 0;
    gWroteBom = false;
    gFilePath[0] = 0;
}

// Tries the document's folder, then the executable's, then %TEMP%. A folder we
// cannot write to (read-only media, network share) simply fails and the next
// candidate is used.
bool OpenFile(const char* docPath) {
    wchar_t candidates[3][MAX_PATH * 2] = {};
    int n = 0;
    if (docPath && docPath[0]) {
        wchar_t docW[MAX_PATH * 2] = {};
        if (MultiByteToWideChar(CP_UTF8, 0, docPath, -1, docW, MAX_PATH * 2) > 0) {
            for (int i = (int)wcslen(docW) - 1; i >= 0; i--) {
                if (docW[i] == L'\\' || docW[i] == L'/') {
                    docW[i] = 0;
                    break;
                }
            }
            if (docW[0]) {
                swprintf_s(candidates[n++], MAX_PATH * 2, L"%s\\SumatraPDF-trace.log", docW);
            }
        }
    }
    wchar_t exeW[MAX_PATH * 2] = {};
    if (GetModuleFileNameW(nullptr, exeW, MAX_PATH * 2) > 0) {
        for (int i = (int)wcslen(exeW) - 1; i >= 0; i--) {
            if (exeW[i] == L'\\') {
                exeW[i] = 0;
                break;
            }
        }
        swprintf_s(candidates[n++], MAX_PATH * 2, L"%s\\SumatraPDF-trace.log", exeW);
    }
    wchar_t tmp[MAX_PATH * 2] = {};
    if (GetTempPathW(MAX_PATH * 2, tmp) > 0) {
        swprintf_s(candidates[n++], MAX_PATH * 2, L"%sSumatraPDF-trace.log", tmp);
    }
    for (int i = 0; i < n; i++) {
        HANDLE h = CreateFileW(candidates[i], FILE_APPEND_DATA,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            continue;
        }
        gFile = h;
        wcscpy_s(gFilePath, MAX_PATH * 2, candidates[i]);
        LARGE_INTEGER size = {};
        gFileBytes = GetFileSizeEx(h, &size) ? (unsigned long long)size.QuadPart : 0;
        gWroteBom = true;
        if (gFileBytes == 0) {
            static const char bom[] = "\xEF\xBB\xBF";
            WriteBytes(bom, 3);
        }
        return true;
    }
    return false;
}

void DrainRing() {
    char batch[kDrainBatch][kLineLen];
    int n = 0;
    int dropped = 0;
    AcquireSRWLockExclusive(&gLock);
    while (n < kDrainBatch && gTail != gHead) {
        memcpy(batch[n], gRing[gTail], kLineLen);
        n++;
        gTail = (gTail + 1) % kRingLines;
    }
    dropped = gDropped;
    gDropped = 0;
    ReleaseSRWLockExclusive(&gLock);
    if (dropped > 0) {
        char line[64];
        snprintf(line, sizeof(line), "(dropped %d lines)", dropped);
        WriteLine(line);
    }
    for (int i = 0; i < n; i++) {
        WriteLine(batch[i]);
    }
    RotateIfNeeded();
}

DWORD WINAPI WorkerProc(void*) {
    ULONG reportedQuiet = 0;
    bool inStall = false;
    ULONGLONG stallStartedMs = 0;
    while (!gStop) {
        WaitForSingleObject(gWake, (DWORD)kTickMs);
        if (gStop) {
            break;
        }

        char docCopy[1024] = {};
        bool docChanged = false;
        AcquireSRWLockExclusive(&gLock);
        docChanged = gDocChanged;
        gDocChanged = false;
        if (docChanged) {
            strncpy(docCopy, gDocPath, sizeof(docCopy) - 1);
        }
        ReleaseSRWLockExclusive(&gLock);
        if (docChanged) {
            CloseFile();
            gFilePath[0] = 0;
            OpenFile(docCopy);
        } else if (gFile == INVALID_HANDLE_VALUE) {
            char cur[1024] = {};
            AcquireSRWLockExclusive(&gLock);
            strncpy(cur, gDocPath, sizeof(cur) - 1);
            ReleaseSRWLockExclusive(&gLock);
            OpenFile(cur);
        }

        DrainRing();

        ULONG hb = (ULONG)InterlockedCompareExchange(&gHeartbeat, 0, 0);
        ULONG now = (ULONG)(GetTickCount64() & 0xFFFFFFFFULL);
        ULONG quiet = now - hb;
        if (quiet >= (ULONG)kStallMs) {
            if (!inStall) {
                inStall = true;
                stallStartedMs = GetTickCount64() - quiet;
            }
            // report as soon as the stall threshold is crossed, then every few
            // seconds while it keeps growing
            if (reportedQuiet == 0 || quiet >= reportedQuiet + (ULONG)kStallRepeatMs) {
                reportedQuiet = quiet;
                char doc[1024] = {};
                AcquireSRWLockExclusive(&gLock);
                strncpy(doc, gDocPath, sizeof(doc) - 1);
                ReleaseSRWLockExclusive(&gLock);
                char line[kLineLen];
                snprintf(line, sizeof(line), "STALL ui_busy_ms=%lu stage=%s doc=%s", (unsigned long)quiet,
                         gStage ? gStage : "?", BaseName(doc));
                WriteStampLine(line);
            }
        } else if (inStall) {
            char line[kLineLen];
            snprintf(line, sizeof(line), "STALL recovered after_ms=%llu stage=%s",
                     (unsigned long long)(GetTickCount64() - stallStartedMs), gStage ? gStage : "?");
            WriteStampLine(line);
            inStall = false;
            reportedQuiet = 0;
        }
    }
    DrainRing();
    CloseFile();
    return 0;
}

}  // namespace

void Start() {
    if (InterlockedCompareExchange(&gStarted, 1, 0) != 0) {
        return;
    }
    gHeartbeat = (LONG)(GetTickCount64() & 0xFFFFFFFFULL);
    gWake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    gThread = CreateThread(nullptr, 0, WorkerProc, nullptr, 0, nullptr);
    if (gThread) {
        SetThreadPriority(gThread, THREAD_PRIORITY_BELOW_NORMAL);
    }
    Event("trace start: pid=%lu", (unsigned long)GetCurrentProcessId());
}

void Heartbeat() {
    InterlockedExchange(&gHeartbeat, (LONG)(GetTickCount64() & 0xFFFFFFFFULL));
}

void Stage(const char* tag) {
    InterlockedExchangePointer((PVOID volatile*)&gStage, (PVOID)tag);
}

void Event(const char* fmt, ...) {
    if (!fmt) {
        return;
    }
    char text[kLineLen];
    char ts[32];
    Timestamp(ts, sizeof(ts));
    int used = snprintf(text, sizeof(text), "%s ", ts);
    if (used < 0 || used >= (int)sizeof(text) - 1) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text + used, sizeof(text) - (size_t)used, fmt, ap);
    va_end(ap);
    PushRaw(text);
}

void SetDocument(const char* utf8Path) {
    if (!utf8Path || !utf8Path[0]) {
        return;
    }
    bool changed = false;
    AcquireSRWLockExclusive(&gLock);
    if (strncmp(gDocPath, utf8Path, sizeof(gDocPath) - 1) != 0) {
        strncpy(gDocPath, utf8Path, sizeof(gDocPath) - 1);
        gDocPath[sizeof(gDocPath) - 1] = 0;
        gDocChanged = true;
        changed = true;
    }
    ReleaseSRWLockExclusive(&gLock);
    if (changed) {
        Event("doc: %s", utf8Path);
        if (gWake) {
            SetEvent(gWake);
        }
    }
}

void Shutdown() {
    if (InterlockedExchange(&gStop, 1) == 1) {
        return;
    }
    if (gWake) {
        SetEvent(gWake);
    }
    if (gThread) {
        WaitForSingleObject(gThread, 2000);
        CloseHandle(gThread);
        gThread = nullptr;
    }
    if (gWake) {
        CloseHandle(gWake);
        gWake = nullptr;
    }
}

}  // namespace tracelog
