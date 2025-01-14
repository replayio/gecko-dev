/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "ProcessRecordReplay.h"

#include "JSControl.h"
#include "mozilla/BasicEvents.h"
#include "mozilla/dom/BrowserChild.h"
#include "mozilla/dom/ScriptSettings.h"
#include "mozilla/Compression.h"
#include "mozilla/CycleCollectedJSContext.h"
#include "mozilla/Maybe.h"
#include "mozilla/Sprintf.h"
#include "mozilla/StaticMutex.h"
#include "mozilla/VsyncDispatcher.h"
#include "nsAppRunner.h"
#include "nsNSSComponent.h"
#include "pratom.h"
#include "nsPrintfCString.h"

#include <fcntl.h>
#include <sys/stat.h>

#ifdef XP_MACOSX
#include "mozilla/MacLaunchHelper.h"
#endif

#ifndef XP_WIN
#include <dlfcn.h>
#include <sys/time.h>
#include <unistd.h>
#else
#include <io.h>
#include <libloaderapi.h>
#endif

extern "C" void RecordReplayOrderDefaultTimeZoneMutex();

namespace mozilla {

namespace image {
  extern void RecordReplayInitializeSurfaceCacheMutex();
}

extern void RecordReplayInitializeTimerThreadWrapperMutex();

namespace recordreplay {

MOZ_NEVER_INLINE void BusyWait() {
  static volatile int value = 1;
  while (value) {
  }
}

///////////////////////////////////////////////////////////////////////////////
// Basic interface
///////////////////////////////////////////////////////////////////////////////

struct JSFilter {
  std::string mFilename;
  unsigned mStartLine = 0;
  unsigned mEndLine = 0;
};

static void ParseJSFilters(const char* aEnv, InfallibleVector<JSFilter>& aFilters);
static bool FilterMatches(const InfallibleVector<JSFilter>& aFilters,
                          const char* aFilename, unsigned aLine);

// Whether to assert on execution progress changes.
static InfallibleVector<JSFilter> gExecutionAsserts;

// Whether to assert on JS values.
static InfallibleVector<JSFilter> gJSAsserts;

static void (*gAttach)(const char* dispatch, const char* buildId);
static void (*gSetApiKey)(const char* apiKey);
static void (*gProfileExecution)(const char* path);
static void (*gAddProfilerEvent)(const char* event, const char* json);
static void (*gLabelExecutableCode)(const void* aCode, size_t aSize, const char* aKind);
static void (*gSetFaultCallback)(FaultCallback aCallback);
static void (*gRecordCommandLineArguments)(int*, char***);
static uintptr_t (*gRecordReplayValue)(const char* why, uintptr_t value);
static void (*gRecordReplayBytes)(const char* why, void* buf, size_t size);
static void (*gPrintVA)(const char* format, va_list args);
static void (*gDiagnosticVA)(const char* format, va_list args);
static void (*gRegisterPointer)(void* ptr);
static void (*gUnregisterPointer)(void* ptr);
static int (*gPointerId)(void* ptr);
static void* (*gIdPointer)(size_t id);
static void (*gAssert)(const char* format, va_list);
static void (*gAssertBytes)(const char* why, const void*, size_t);
static void (*gSaveRecording)(const char* dir);
static void (*gRememberRecording)();
static void (*gFinishRecording)();
static uint64_t* (*gProgressCounter)();
static void (*gSetProgressCallback)(void (*aCallback)(uint64_t));
static void (*gEnableProgressCheckpoints)();
static void (*gProgressReached)();
static void (*gSetTrackObjectsCallback)(void (*aCallback)(bool));
static void (*gBeginPassThroughEvents)();
static void (*gEndPassThroughEvents)();
static bool (*gAreEventsPassedThrough)();
static void (*gBeginDisallowEvents)();
static void (*gEndDisallowEvents)();
static bool (*gAreEventsDisallowed)();
static bool (*gHasDivergedFromRecording)();
static bool (*gAllowSideEffects)();
static void (*gRecordReplayNewCheckpoint)();
static bool (*gRecordReplayIsReplaying)();
static int (*gCreateOrderedLock)(const char* aName);
static void (*gOrderedLock)(int aLock);
static void (*gOrderedUnlock)(int aLock);
static void (*gOnMouseEvent)(const char* aKind, size_t aClientX, size_t aClientY);
static void (*gOnKeyEvent)(const char* aKind, const char* aKey);
static void (*gOnNavigationEvent)(const char* aKind, const char* aUrl);
static const char* (*gGetRecordingId)();
static void (*gProcessRecording)();
static void (*gSetCrashReasonCallback)(const char* (*aCallback)());
static void (*gInvalidateRecording)(const char* aFormat, ...);
static void (*gSetCrashNote)(const char* aNote);
static void (*gNotifyActivity)();
static void (*gNewStableHashTable)(const void* aTable, KeyEqualsEntryCallback aKeyEqualsEntry, void* aPrivate);
static void (*gMoveStableHashTable)(const void* aTableSrc, const void* aTableDst);
static void (*gDeleteStableHashTable)(const void* aTable);
static uint32_t (*gLookupStableHashCode)(const void* aTable, const void* aKey, uint32_t aUnstableHashCode,
                                         bool* aFoundMatch);
static void (*gStableHashTableAddEntryForLastLookup)(const void* aTable, const void* aEntry);
static void (*gStableHashTableMoveEntry)(const void* aTable, const void* aEntrySrc, const void* aEntryDst);
static void (*gStableHashTableDeleteEntry)(const void* aTable, const void* aEntry);
static bool (*gIsRecordingCreated)();
static bool (*gWaitForRecordingCreated)();

#ifndef XP_WIN
static void (*gAddOrderedPthreadMutex)(const char* aName, pthread_mutex_t* aMutex);
typedef void* DriverHandle;
#else
static void (*gAddOrderedCriticalSection)(const char* aName, void* aCS);
static void (*gAddOrderedSRWLock)(const char* aName, void* aLock);
typedef HMODULE DriverHandle;
#endif

static DriverHandle gDriverHandle;

void LoadSymbolInternal(const char* name, void** psym, bool aOptional) {
#ifndef XP_WIN
  *psym = dlsym(gDriverHandle, name);
#else
  *psym = BitwiseCast<void*>(GetProcAddress(gDriverHandle, name));
#endif
  if (!*psym && !aOptional) {
    fprintf(stderr, "Could not find %s in Record Replay driver, crashing.\n", name);
    MOZ_CRASH();
  }
}

// This is called when the process crashes to return any reason why Gecko is crashing.
static const char* GetCrashReason() {
  return gMozCrashReason;
}

// Do any special Gecko configuration to get it ready for recording/replaying.
static void ConfigureGecko() {
  // Don't create a stylo thread pool when recording or replaying.
  putenv((char*)"STYLO_THREADS=1");

  // StaticMutex objects initialize their underlying mutex the first time they
  // are locked. If two threads are racing to do this initialization then it
  // can happen at different points when recording vs. replaying, and we get
  // mismatches that cause replaying failures. To work around this we
  // initialize these mutexes explicitly here so that it happens at a
  // consistent point in time.
  image::RecordReplayInitializeSurfaceCacheMutex();
  RecordReplayInitializeTimerThreadWrapperMutex();

  // Order statically allocated mutex in intl code.
  RecordReplayOrderDefaultTimeZoneMutex();

#ifdef XP_WIN
  // Make sure NSS is always initialized in case it gets used while generating paint data.
  EnsureNSSInitializedChromeOrContent();
#endif
}

extern char gRecordReplayDriver[];
extern int gRecordReplayDriverSize;
extern char gBuildId[];

const char* GetBuildId() {
  return gBuildId;
}

static const char* GetTempDirectory() {
#ifndef XP_WIN
  const char* tmpdir = getenv("TMPDIR");
  return tmpdir ? tmpdir : "/tmp";
#else
  return getenv("TEMP");
#endif
}

static DriverHandle DoLoadDriverHandle(const char* aPath, bool aPrintError = true) {
#ifndef XP_WIN
  void* handle = dlopen(aPath, RTLD_LAZY);
  if (!handle && aPrintError) {
    char* error = dlerror();
    fprintf(stderr, "DoLoadDriverHandle: dlopen failed %s: %s\n", aPath, error ? error : "<no error>");
  }
  return handle;
#else
  HMODULE handle = LoadLibraryA(aPath);
  if (!handle && aPrintError) {
    fprintf(stderr, "DoLoadDriverHandle: LoadLibraryA failed %s: %u\n", aPath, GetLastError());
  }
  return handle;
#endif
}

static DriverHandle OpenDriverHandle() {
  const char* driver = getenv("RECORD_REPLAY_DRIVER");
  if (driver) {
    return DoLoadDriverHandle(driver);
  }

  const char* tmpdir = GetTempDirectory();
  if (!tmpdir) {
    fprintf(stderr, "Can't figure out temporary directory, can't create driver.\n");
    return nullptr;
  }

  char filename[1024];
#ifndef XP_WIN
  snprintf(filename, sizeof(filename), "%s/recordreplay-%s.so", tmpdir, gBuildId);
#else
  snprintf(filename, sizeof(filename), "%s\\recordreplay-%s.dll", tmpdir, gBuildId);
#endif

  DriverHandle handle = DoLoadDriverHandle(filename, /* aPrintError */ false);
  if (handle) {
    return handle;
  }

  char tmpFilename[1024];
#ifndef XP_WIN
  snprintf(tmpFilename, sizeof(tmpFilename), "%s/recordreplay.so-XXXXXX", tmpdir);
  int fd = mkstemp(tmpFilename);
#else
  int fd;
  for (int i = 0; i < 10; i++) {
    snprintf(tmpFilename, sizeof(tmpFilename), "%s\\recordreplay.dll-XXXXXX", tmpdir);
    _mktemp(tmpFilename);
    fd = _open(tmpFilename, O_CREAT | O_TRUNC | O_WRONLY | O_BINARY);
    if (fd >= 0) {
      break;
    }
  }
  #define write _write
  #define close _close
#endif
  if (fd < 0) {
    fprintf(stderr, "mkstemp failed, can't create driver.\n");
    return nullptr;
  }

  int nbytes = write(fd, gRecordReplayDriver, gRecordReplayDriverSize);
  if (nbytes != gRecordReplayDriverSize) {
    fprintf(stderr, "write to driver temporary file failed, can't create driver.\n");
    return nullptr;
  }

  close(fd);

#ifdef XP_MACOSX
  // Strip any quarantine flag on the written file, if necessary, so that
  // the file can be run or loaded into a process. macOS quarantines any
  // files created by the browser even if they are related to the update
  // process.
  char* args[] = {
    (char*)"/usr/bin/xattr",
    (char*)"-d",
    (char*)"com.apple.quarantine",
    tmpFilename,
  };
  pid_t pid;
  LaunchChildMac(4, args, &pid);
#endif // XP_MACOSX

  int rv = rename(tmpFilename, filename);
  if (rv < 0) {
    fprintf(stderr, "renaming temporary driver failed\n");
  }

  return DoLoadDriverHandle(filename);
}

static void FreeCallback(void* aPtr) {
  // This may be calling into jemalloc, which won't happen on all platforms
  // if the driver tries to call free() directly.
  free(aPtr);
}

bool gRecordAllContent;
const char* gRecordingUnsupported;

static const char* GetRecordingUnsupportedReason() {
#ifdef XP_MACOSX
  // Using __builtin_available is not currently supported before attaching to
  // the record/replay driver, as it interacts with the system in mildly
  // complicated ways. Instead, we use this stupid hack to detect whether we
  // are replaying, in which case recording is certainly supported.
  const char* env = getenv("RECORD_REPLAY_DRIVER");
  if (env && !strcmp(env, "recordreplay-driver")) {
    return nullptr;
  }

  if (__builtin_available(macOS 10.14, *)) {
    return nullptr;
  }

  return "Recording requires macOS 10.14 or higher";
#else
  return nullptr;
#endif
}

// If the profiler is enabled via the environment, start it.
static void MaybeStartProfiling() {
  const char* directory = getenv("RECORD_REPLAY_PROFILE_DIRECTORY");
  if (!directory) {
    return;
  }

  nsPrintfCString path("%s%cprofile-%d.log", directory, PR_GetDirectorySeparator(), rand());

  gProfileExecution(path.get());
  gIsProfiling = true;
}

// This can be set while recording to pretend we're not recording when other
// places in gecko check to see if they need to change their behavior.
// This is used with the record/replay profiler to understand the performance
// effects of these changes to gecko's behavior. When this is set, the resulting
// recording will not be usable.
static bool gPretendNotRecording = false;

// Whether the recorder will be directly uploading the recording, vs. writing it to disk.
static bool gUploadingRecording = false;

extern "C" {

MOZ_EXPORT void RecordReplayInterface_Initialize(int* aArgc, char*** aArgv) {
  gRecordingUnsupported = GetRecordingUnsupportedReason();
  if (gRecordingUnsupported) {
    return;
  }

  // Parse command line options for the process kind and recording file.
  Maybe<const char*> dispatchAddress;
  int argc = *aArgc;
  char** argv = *aArgv;
  for (int i = 0; i < argc; i++) {
    if (!strcmp(argv[i], "-recordReplayDispatch")) {
      MOZ_RELEASE_ASSERT(dispatchAddress.isNothing() && i + 1 < argc);
      const char* arg = argv[i + 1];

      // The special dispatch address "*" is used to indicate that we should
      // save the recording itself to disk.
      dispatchAddress.emplace(strcmp(arg, "*") ? arg : nullptr);
    }
  }
  if (!dispatchAddress.isSome()) {
    return;
  }

  Maybe<std::string> apiKey;
  // this environment variable is set by server/actors/replay/connection.js
  // to contain the API key or user token
  const char* val = getenv("RECORD_REPLAY_AUTH");
  if (val && val[0]) {
    apiKey.emplace(val);
    // Unsetting the env var will make the variable unavailable via
    // getenv and such, and also mutates the 'environ' global, so
    // by the time gAttach runs, it will have no idea that this value
    // existed and won't capture it in the recording itself, which
    // is ideal for security.
#ifdef XP_WIN
    MOZ_RELEASE_ASSERT(!_putenv("RECORD_REPLAY_AUTH="));
    MOZ_RELEASE_ASSERT(!_putenv("RECORD_REPLAY_API_KEY="));
#else
    MOZ_RELEASE_ASSERT(!unsetenv("RECORD_REPLAY_AUTH"));
    MOZ_RELEASE_ASSERT(!unsetenv("RECORD_REPLAY_API_KEY"));
#endif
  }

  gDriverHandle = OpenDriverHandle();
  if (!gDriverHandle) {
    fprintf(stderr, "Loading recorder library failed.\n");
    return;
  }

  LoadSymbol("RecordReplayAttach", gAttach);
  LoadSymbol("RecordReplaySetApiKey", gSetApiKey);
  LoadSymbol("RecordReplayProfileExecution", gProfileExecution);
  LoadSymbol("RecordReplayAddProfilerEvent", gAddProfilerEvent);
  LoadSymbol("RecordReplayLabelExecutableCode", gLabelExecutableCode);
  LoadSymbol("RecordReplaySetFaultCallback", gSetFaultCallback);
  LoadSymbol("RecordReplayRecordCommandLineArguments",
             gRecordCommandLineArguments);
  LoadSymbol("RecordReplayValue", gRecordReplayValue);
  LoadSymbol("RecordReplayBytes", gRecordReplayBytes);
  LoadSymbol("RecordReplayPrint", gPrintVA);
  LoadSymbol("RecordReplayDiagnostic", gDiagnosticVA);
  LoadSymbol("RecordReplaySaveRecording", gSaveRecording);
  LoadSymbol("RecordReplayRememberRecording", gRememberRecording);
  LoadSymbol("RecordReplayFinishRecording", gFinishRecording);
  LoadSymbol("RecordReplayRegisterPointer", gRegisterPointer);
  LoadSymbol("RecordReplayUnregisterPointer", gUnregisterPointer);
  LoadSymbol("RecordReplayPointerId", gPointerId);
  LoadSymbol("RecordReplayIdPointer", gIdPointer);
  LoadSymbol("RecordReplayAssert", gAssert);
  LoadSymbol("RecordReplayAssertBytes", gAssertBytes);
  LoadSymbol("RecordReplayProgressCounter", gProgressCounter);
  LoadSymbol("RecordReplaySetProgressCallback", gSetProgressCallback);
  LoadSymbol("RecordReplayEnableProgressCheckpoints", gEnableProgressCheckpoints);
  LoadSymbol("RecordReplayProgressReached", gProgressReached);
  LoadSymbol("RecordReplaySetTrackObjectsCallback", gSetTrackObjectsCallback);
  LoadSymbol("RecordReplayBeginPassThroughEvents", gBeginPassThroughEvents);
  LoadSymbol("RecordReplayEndPassThroughEvents", gEndPassThroughEvents);
  LoadSymbol("RecordReplayAreEventsPassedThrough", gAreEventsPassedThrough);
  LoadSymbol("RecordReplayBeginDisallowEvents", gBeginDisallowEvents);
  LoadSymbol("RecordReplayEndDisallowEvents", gEndDisallowEvents);
  LoadSymbol("RecordReplayAreEventsDisallowed", gAreEventsDisallowed);
  LoadSymbol("RecordReplayHasDivergedFromRecording", gHasDivergedFromRecording);
  LoadSymbol("RecordReplayAllowSideEffects", gAllowSideEffects);
  LoadSymbol("RecordReplayNewCheckpoint", gRecordReplayNewCheckpoint);
  LoadSymbol("RecordReplayIsReplaying", gRecordReplayIsReplaying);
  LoadSymbol("RecordReplayCreateOrderedLock", gCreateOrderedLock);
  LoadSymbol("RecordReplayOrderedLock", gOrderedLock);
  LoadSymbol("RecordReplayOrderedUnlock", gOrderedUnlock);
  LoadSymbol("RecordReplayOnMouseEvent", gOnMouseEvent);
  LoadSymbol("RecordReplayOnKeyEvent", gOnKeyEvent);
  LoadSymbol("RecordReplayOnNavigationEvent", gOnNavigationEvent);
  LoadSymbol("RecordReplayGetRecordingId", gGetRecordingId);
  LoadSymbol("RecordReplayProcessRecording", gProcessRecording);
  LoadSymbol("RecordReplaySetCrashReasonCallback", gSetCrashReasonCallback);
  LoadSymbol("RecordReplayInvalidateRecording", gInvalidateRecording);
  LoadSymbol("RecordReplaySetCrashNote", gSetCrashNote, /* aOptional */ true);
  LoadSymbol("RecordReplayNotifyActivity", gNotifyActivity);
  LoadSymbol("RecordReplayNewStableHashTable", gNewStableHashTable);
  LoadSymbol("RecordReplayMoveStableHashTable", gMoveStableHashTable);
  LoadSymbol("RecordReplayDeleteStableHashTable", gDeleteStableHashTable);
  LoadSymbol("RecordReplayLookupStableHashCode", gLookupStableHashCode);
  LoadSymbol("RecordReplayStableHashTableAddEntryForLastLookup", gStableHashTableAddEntryForLastLookup);
  LoadSymbol("RecordReplayStableHashTableMoveEntry", gStableHashTableMoveEntry);
  LoadSymbol("RecordReplayStableHashTableDeleteEntry", gStableHashTableDeleteEntry);
  LoadSymbol("RecordReplayIsRecordingCreated", gIsRecordingCreated);
  LoadSymbol("RecordReplayWaitForRecordingCreated", gWaitForRecordingCreated);

  if (apiKey) {
    gSetApiKey(apiKey->c_str());
  }

#ifndef XP_WIN
  LoadSymbol("RecordReplayAddOrderedPthreadMutex", gAddOrderedPthreadMutex);
#else
  LoadSymbol("RecordReplayAddOrderedCriticalSection", gAddOrderedCriticalSection);
  LoadSymbol("RecordReplayAddOrderedSRWLock", gAddOrderedSRWLock);
#endif

  gAttach(*dispatchAddress, gBuildId);

  if (TestEnv("RECORD_ALL_CONTENT")) {
    gRecordAllContent = true;

    // We only save information about the recording to disk when recording all
    // content. We don't want to save this information when the user explicitly
    // started recording --- they won't use the recording CLI tool
    // (https://github.com/RecordReplay/recordings-cli) afterwards to inspect
    // the recording, and we don't want to leak recording IDs to disk in an
    // unexpected way.
    if (gSaveRecording) {
      gSaveRecording(nullptr);
    }
  }

  js::InitializeJS();
  InitializeGraphics();

  if (TestEnv("RECORD_REPLAY_PRETEND_NOT_RECORDING")) {
    gPretendNotRecording = true;
  }

  if (!gPretendNotRecording) {
    gIsRecordingOrReplaying = true;
    gIsRecording = !gRecordReplayIsReplaying();
    gIsReplaying = gRecordReplayIsReplaying();
  }

  void (*SetFreeCallback)(void (*aCallback)(void*));
  LoadSymbol("RecordReplaySetFreeCallback", SetFreeCallback);
  SetFreeCallback(FreeCallback);

  ParseJSFilters("RECORD_REPLAY_RECORD_EXECUTION_ASSERTS", gExecutionAsserts);
  ParseJSFilters("RECORD_REPLAY_RECORD_JS_ASSERTS", gJSAsserts);

  gRecordCommandLineArguments(aArgc, aArgv);
  gSetCrashReasonCallback(GetCrashReason);

  gUploadingRecording = RecordReplayValue("UploadingRecording", !!*dispatchAddress);

  // Unless disabled via the environment, pre-process all created recordings so
  // that they will load faster after saving the recording.
  if (!TestEnv("RECORD_REPLAY_DONT_PROCESS_RECORDINGS") &&
      !TestEnv("RECORD_ALL_CONTENT")) {
    gProcessRecording();
  }

  if (!gPretendNotRecording) {
    ConfigureGecko();
  }
  MaybeStartProfiling();
}

MOZ_EXPORT size_t
RecordReplayInterface_InternalRecordReplayValue(const char* aWhy, size_t aValue) {
  return gRecordReplayValue(aWhy, aValue);
}

MOZ_EXPORT void RecordReplayInterface_InternalRecordReplayBytes(const char* aWhy,
                                                                void* aData,
                                                                size_t aSize) {
  gRecordReplayBytes(aWhy, aData, aSize);
}

MOZ_EXPORT void RecordReplayInterface_InternalInvalidateRecording(
    const char* aWhy) {
  gInvalidateRecording("%s", aWhy);
}

MOZ_EXPORT void RecordReplayInterface_InternalRecordReplayAssert(
    const char* aFormat, va_list aArgs) {
  gAssert(aFormat, aArgs);
}

MOZ_EXPORT void RecordReplayInterface_InternalRecordReplayAssertBytes(
    const void* aData, size_t aSize) {
  gAssertBytes("Bytes", aData, aSize);
}

MOZ_EXPORT void RecordReplayAssertFromC(const char* aFormat, ...) {
  if (IsRecordingOrReplaying()) {
    va_list args;
    va_start(args, aFormat);
    gAssert(aFormat, args);
    va_end(args);
  }
}

MOZ_EXPORT void RecordReplayInterface_InternalRegisterThing(void* aThing) {
  gRegisterPointer(aThing);
}

MOZ_EXPORT void RecordReplayInterface_InternalUnregisterThing(void* aThing) {
  gUnregisterPointer(aThing);
}

MOZ_EXPORT size_t RecordReplayInterface_InternalThingIndex(void* aThing) {
  return gPointerId(aThing);
}

MOZ_EXPORT void* RecordReplayInterface_InternalIndexThing(size_t aId) {
  return gIdPointer(aId);
}

MOZ_EXPORT void RecordReplayInterface_InternalAssertScriptedCaller(const char* aWhy) {
  JS::AutoFilename filename;
  unsigned lineno;
  unsigned column;
  JSContext* cx = nullptr;
  if (NS_IsMainThread() && CycleCollectedJSContext::Get()) {
    cx = dom::danger::GetJSContext();
  }
  if (cx && JS::DescribeScriptedCaller(cx, &filename, &lineno, &column)) {
    RecordReplayAssert("%s %s:%u:%u", aWhy, filename.get(), lineno, column);
  } else {
    RecordReplayAssert("%s NoScriptedCaller", aWhy);
  }
}

MOZ_EXPORT void RecordReplayInterface_InternalNotifyActivity() {
  gNotifyActivity();
}

MOZ_EXPORT void RecordReplayInterface_ExecutionProgressHook(unsigned aSourceId, const char* aFilename, unsigned aLineno,
                                                            unsigned aColumn) {
  if (FilterMatches(gExecutionAsserts, aFilename, aLineno)) {
    RecordReplayAssert("ExecutionProgress %u:%s:%u:%u", aSourceId, aFilename, aLineno, aColumn);
  }
}

MOZ_EXPORT bool RecordReplayInterface_ShouldEmitRecordReplayAssert(const char* aFilename,
                                                                   unsigned aLineno,
                                                                   unsigned aColumn) {
  return FilterMatches(gJSAsserts, aFilename, aLineno);
}

MOZ_EXPORT void RecordReplayInterface_InternalPrintLog(const char* aFormat,
                                                       va_list aArgs) {
  gPrintVA(aFormat, aArgs);
}

MOZ_EXPORT void RecordReplayInterface_InternalDiagnostic(const char* aFormat,
                                                         va_list aArgs) {
  gDiagnosticVA(aFormat, aArgs);
}

MOZ_EXPORT ProgressCounter* RecordReplayInterface_ExecutionProgressCounter() {
  return gProgressCounter();
}

MOZ_EXPORT void RecordReplayInterface_AdvanceExecutionProgressCounter() {
  ++*gProgressCounter();
}

MOZ_EXPORT void RecordReplayInterface_SetExecutionProgressCallback(void (*aCallback)(uint64_t)) {
  gSetProgressCallback(aCallback);
  gEnableProgressCheckpoints();
}

MOZ_EXPORT void RecordReplayInterface_ExecutionProgressReached() {
  gProgressReached();
}

MOZ_EXPORT void RecordReplayInterface_SetTrackObjectsCallback(void (*aCallback)(bool)) {
  gSetTrackObjectsCallback(aCallback);
}

MOZ_EXPORT void RecordReplayInterface_InternalBeginPassThroughThreadEvents() {
  gBeginPassThroughEvents();
}

MOZ_EXPORT void RecordReplayInterface_InternalEndPassThroughThreadEvents() {
  gEndPassThroughEvents();
}

MOZ_EXPORT bool RecordReplayInterface_InternalAreThreadEventsPassedThrough() {
  return gAreEventsPassedThrough();
}

MOZ_EXPORT void RecordReplayInterface_InternalBeginDisallowThreadEvents() {
  gBeginDisallowEvents();
}

MOZ_EXPORT void RecordReplayInterface_InternalEndDisallowThreadEvents() {
  gEndDisallowEvents();
}

MOZ_EXPORT bool RecordReplayInterface_InternalAreThreadEventsDisallowed() {
  return gAreEventsDisallowed();
}

MOZ_EXPORT bool RecordReplayInterface_InternalHasDivergedFromRecording() {
  return gHasDivergedFromRecording();
}

MOZ_EXPORT bool RecordReplayInterface_InternalAllowSideEffects() {
  return gAllowSideEffects();
}

MOZ_EXPORT int RecordReplayInterface_InternalCreateOrderedLock(const char* aName) {
  return gCreateOrderedLock(aName);
}

int RecordReplayCreateOrderedLock(const char* aName) {
  if (gCreateOrderedLock) {
    return gCreateOrderedLock(aName);
  }
  return 0;
}

MOZ_EXPORT void RecordReplayInterface_InternalOrderedLock(int aLock) {
  gOrderedLock(aLock);
}

void RecordReplayOrderedLock(int aLock) {
  if (gOrderedLock) {
    gOrderedLock(aLock);
  }
}

MOZ_EXPORT void RecordReplayInterface_InternalOrderedUnlock(int aLock) {
  gOrderedUnlock(aLock);
}

void RecordReplayOrderedUnlock(int aLock) {
  if (gOrderedUnlock) {
    gOrderedUnlock(aLock);
  }
}

#ifndef XP_WIN

MOZ_EXPORT void RecordReplayInterface_InternalAddOrderedPthreadMutex(const char* aName,
                                                                     pthread_mutex_t* aMutex) {
  gAddOrderedPthreadMutex(aName, aMutex);
}

MOZ_EXPORT void RecordReplayAddOrderedPthreadMutexFromC(const char* aName, pthread_mutex_t* aMutex) {
  if (IsRecordingOrReplaying()) {
    gAddOrderedPthreadMutex(aName, aMutex);
  }
}

#else // XP_WIN

MOZ_EXPORT void RecordReplayInterface_InternalAddOrderedCriticalSection(const char* aName, void* aCS) {
  gAddOrderedCriticalSection(aName, aCS);
}

MOZ_EXPORT void RecordReplayAddOrderedCriticalSectionFromC(const char* aName, PCRITICAL_SECTION aCS) {
  if (IsRecordingOrReplaying()) {
    gAddOrderedCriticalSection(aName, aCS);
  }
}

MOZ_EXPORT void RecordReplayInterface_InternalAddOrderedSRWLock(const char* aName, void* aLock) {
  gAddOrderedSRWLock(aName, aLock);
}

#endif // XP_WIN

static Vector<const char*> gCrashNotes;

MOZ_EXPORT void RecordReplayInterface_InternalPushCrashNote(const char* aNote) {
  if (NS_IsMainThread()) {
    (void) gCrashNotes.append(aNote);
    if (gSetCrashNote) {
      gSetCrashNote(aNote);
    }
  }
}

MOZ_EXPORT void RecordReplayInterface_InternalPopCrashNote() {
  if (NS_IsMainThread()) {
    MOZ_RELEASE_ASSERT(gCrashNotes.length());
    gCrashNotes.popBack();
    if (gSetCrashNote) {
      gSetCrashNote(gCrashNotes.length() ? gCrashNotes.back() : nullptr);
    }
  }
}

MOZ_EXPORT void RecordReplayInterface_AddProfilerEvent(const char* aEvent, const char* aJSON) {
  if (gIsRecordingOrReplaying || gIsProfiling) {
    gAddProfilerEvent(aEvent, aJSON);
  }
}

MOZ_EXPORT void RecordReplayInterface_LabelExecutableCode(const void* aCode, size_t aSize, const char* aKind) {
  if (gIsRecordingOrReplaying || gIsProfiling) {
    gLabelExecutableCode(aCode, aSize, aKind);
  }
}

MOZ_EXPORT void RecordReplayInterface_SetFaultCallback(FaultCallback aCallback) {
  if (gIsRecordingOrReplaying) {
    gSetFaultCallback(aCallback);
  }
}

}  // extern "C"

bool IsRecordingCreated() {
  return gIsRecordingCreated();
}

bool IsUploadingRecording() {
  return gUploadingRecording;
}

const char* GetRecordingId() {
  return gGetRecordingId();
}

static void ParseJSFilters(const char* aEnv, InfallibleVector<JSFilter>& aFilters) {
  const char* value = getenv(aEnv);
  if (!value) {
    return;
  }

  if (!strcmp(value, "*")) {
    JSFilter filter;
    filter.mFilename = value;
    aFilters.append(filter);
  }

  while (true) {
    JSFilter filter;

    const char* end = strchr(value, '@');
    if (!end) {
      break;
    }

    filter.mFilename = std::string(value, end - value);
    value = end + 1;

    end = strchr(value, '@');
    if (!end) {
      break;
    }

    filter.mStartLine = atoi(value);
    value = end + 1;

    filter.mEndLine = atoi(value);

    PrintLog("ParseJSFilter %s %s %u %u", aEnv,
             filter.mFilename.c_str(), filter.mStartLine, filter.mEndLine);
    aFilters.append(filter);

    end = strchr(value, '@');
    if (!end) {
      break;
    }

    value = end + 1;
  }
}

static bool FilterMatches(const InfallibleVector<JSFilter>& aFilters,
                          const char* aFilename, unsigned aLine) {
  for (const JSFilter& filter : aFilters) {
    if (filter.mFilename == "*") {
      return true;
    }
    if (strstr(aFilename, filter.mFilename.c_str()) &&
        aLine >= filter.mStartLine &&
        aLine <= filter.mEndLine) {
      return true;
    }
  }
  return false;
}

const char* CurrentFirefoxVersion() {
  return "91.0";
}

static bool gHasCheckpoint = false;

bool HasCheckpoint() {
  return gHasCheckpoint;
}

// Note: This should be called even if we aren't recording/replaying, to report
// cases where recording is unsupported to the UI process.
void CreateCheckpoint() {
  if (!IsRecordingOrReplaying()) {
    if (gRecordingUnsupported) {
      js::EnsureModuleInitialized();
      js::SendRecordingUnsupported(gRecordingUnsupported);
    }
    return;
  }

  js::EnsureModuleInitialized();
  js::MaybeSendRecordingUnusable();

  gRecordReplayNewCheckpoint();
  gHasCheckpoint = true;

  // When recording all content, we won't remember the recording until it has loaded
  // some interesting source. See Method_OnNewSource. Otherwise we want to make sure
  // the recording has at least one checkpoint, which won't be the case for
  // preallocated recording processes which aren't in use yet.
  if (!gRecordAllContent) {
    RememberRecording();
  }
}

void MaybeCreateCheckpoint() {
  // This is called at the top of the event loop, and the process might not be
  // fully initialized. CreateCheckpoint() is only called after the process has
  // been fully initialized, and we don't want any checkpoints before then.
  if (HasCheckpoint()) {
    gRecordReplayNewCheckpoint();
  }
}

void RememberRecording() {
  gRememberRecording();
}

static bool gTearingDown;

void FinishRecording() {
  // SendRecordingFinished will inform the parent process if the recording is
  // finished or unusable, but we don't want to do that until we are sure
  // that the connection has either:
  //  a) opened and created the recording in our DB
  //  b) failed and marked the recording as unusable
  gWaitForRecordingCreated();

  js::SendRecordingFinished();

  gFinishRecording();

  // RecordReplayFinishRecording() does not return until the recording has been
  // fully uploaded. The ContentParent will not kill this process after
  // finishing the recording, so we have to it ourselves.
  PrintLog("Recording finished, exiting.");

  // Use abort to avoid running static initializers.
  gTearingDown = true;
  abort();
}

bool IsTearingDownProcess() {
  return gTearingDown;
}

void OnMouseEvent(dom::BrowserChild* aChild, const WidgetMouseEvent& aEvent) {
  if (!gHasCheckpoint) {
    return;
  }

  const char* kind = nullptr;
  if (aEvent.mMessage == eMouseDown) {
    kind = "mousedown";
  } else if (aEvent.mMessage == eMouseMove) {
    kind = "mousemove";
  }

  if (kind) {
    gOnMouseEvent(kind, aEvent.mRefPoint.x, aEvent.mRefPoint.y);
  }
}

void OnKeyboardEvent(dom::BrowserChild* aChild, const WidgetKeyboardEvent& aEvent) {
  if (!gHasCheckpoint) {
    return;
  }

  const char* kind = nullptr;
  if (aEvent.mMessage == eKeyPress) {
    kind = "keypress";
  } else if (aEvent.mMessage == eKeyDown) {
    kind = "keydown";
  } else if (aEvent.mMessage == eKeyUp) {
    kind = "keyup";
  }

  if (kind) {
    nsAutoString key;
    aEvent.GetDOMKeyName(key);

    gOnKeyEvent(kind, PromiseFlatCString(NS_ConvertUTF16toUTF8(key)).get());
  }
}

static nsCString gLastLocationURL;

void OnLocationChange(dom::BrowserChild* aChild, nsIURI* aLocation, uint32_t aFlags) {
  if (!gHasCheckpoint) {
    return;
  }

  nsCString url;
  if (NS_FAILED(aLocation->GetSpec(url))) {
    return;
  }

  // When beginning recording, this function is generally called in the
  // following pattern:
  // 1. Session history is applied from previous non-recording process.
  // 2. An initial about:blank page is loaded into the document
  // 3. Navigation notifications as you'd expect then begin to happen.
  //
  // Since we only care about that third step, we explicitly ignore
  // all location changes before about:blank, and also ignore about: URLs
  // entirely.
  if (gLastLocationURL.IsEmpty()) {
    if (!url.EqualsLiteral("about:blank")) {
      return;
    }

    gLastLocationURL = url;
  }

  // All browser children load with an initial "about:blank" page before loading
  // the overall document. There also also cases like "about:neterror" that may
  // pop up if the browser tries and fails to navigate for some reason.
  // Rather than restrict specifically those, we broadly reject all about: URLs
  // since they shouldn't come up often anyway.
  if (aLocation->SchemeIs("about")) {
    return;
  }

  // The browser internally may do replaceState with the same URL, so we want to
  // filter those out. This means we also won't register location changes for
  // explicit replaceState calls, but that's probably closer to what users will
  // expect anyway.
  if ((aFlags & nsIWebProgressListener::LOCATION_CHANGE_SAME_DOCUMENT) &&
      gLastLocationURL.Equals(url)) {
    return;
  }

  gOnNavigationEvent(nullptr, url.get());
  gLastLocationURL = url;
}

void NewStableHashTable(const void* aTable, KeyEqualsEntryCallback aKeyEqualsEntry, void* aPrivate) {
  if (IsRecordingOrReplaying()) {
    gNewStableHashTable(aTable, aKeyEqualsEntry, aPrivate);
  }
}

void MoveStableHashTable(const void* aTableSrc, const void* aTableDst) {
  if (IsRecordingOrReplaying()) {
    gMoveStableHashTable(aTableSrc, aTableDst);
  }
}

void DeleteStableHashTable(const void* aTable) {
  if (IsRecordingOrReplaying()) {
    gDeleteStableHashTable(aTable);
  }
}

uint32_t LookupStableHashCode(const void* aTable, const void* aKey, uint32_t aUnstableHashCode,
                              bool* aFoundMatch) {
  MOZ_RELEASE_ASSERT(IsRecordingOrReplaying());
  return gLookupStableHashCode(aTable, aKey, aUnstableHashCode, aFoundMatch);
}

void StableHashTableAddEntryForLastLookup(const void* aTable, const void* aEntry) {
  if (IsRecordingOrReplaying()) {
    gStableHashTableAddEntryForLastLookup(aTable, aEntry);
  }
}

void StableHashTableMoveEntry(const void* aTable, const void* aEntrySrc, const void* aEntryDst) {
  if (IsRecordingOrReplaying()) {
    gStableHashTableMoveEntry(aTable, aEntrySrc, aEntryDst);
  }
}

void StableHashTableDeleteEntry(const void* aTable, const void* aEntry) {
  if (IsRecordingOrReplaying()) {
    gStableHashTableDeleteEntry(aTable, aEntry);
  }
}

}  // namespace recordreplay
}  // namespace mozilla
