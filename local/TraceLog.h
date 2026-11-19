#ifndef LOCAL_TRACE_LOG_H_
#define LOCAL_TRACE_LOG_H_

/* Background trace log for this build.

   The UI thread only ever appends a short line to a small in-memory ring; a
   worker thread owns the file and writes the ring out, so a frozen UI still
   leaves a record behind. The worker also watches a heartbeat the UI thread
   bumps once per message: when it stops moving it logs how long the UI has been
   stuck and what it was doing at the time (the "stage" tag last set).

   Only Win32 and the C runtime are used here, so this can be included from any
   translation unit; upstream headers have no include guards and must not be
   pulled in.

   The log lives next to the document being opened (SumatraPDF-trace.log), with
   the executable's folder and %TEMP% as fallbacks. It rotates at 4 MB. */

namespace tracelog {

// Starts the worker. Safe to call more than once; later calls are ignored.
void Start();

// Called once per message-loop iteration: proves the UI thread is alive.
void Heartbeat();

// Marks what the UI thread is busy with (a static string, not copied).
void Stage(const char* tag);

// Queues one line. Cheap and safe to call from any thread.
void Event(const char* fmt, ...);

// Sets the document whose folder receives the log, and logs the switch.
// utf8Path may be null to keep the current file.
void SetDocument(const char* utf8Path);

void Shutdown();

}  // namespace tracelog

#endif  // LOCAL_TRACE_LOG_H_
