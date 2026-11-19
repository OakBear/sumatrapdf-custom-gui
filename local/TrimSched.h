#ifndef LOCAL_TRIM_SCHED_H_
#define LOCAL_TRIM_SCHED_H_

/* True while the auto-crop detection still owes work to the current layout: it
   stopped because its per-call time budget ran out, and the message loop should
   call DisplayModel::EnsureTrimEmptyMarginsForVisiblePages() again when the UI
   has nothing else to do. Defined in DisplayModel.cpp. */
bool TrimDetectionPending();

#endif  // LOCAL_TRIM_SCHED_H_
