#pragma once

// BBHOST_SF_PROBE=1: where Scaleform's memory goes. Hooks on the heap
// engine's grow path (0x233c660) log each heap's footprint against its limit
// and the free space of the Dantelion heaps under it; a hook on the global
// heap's AllocAutoHeap (0x23389c0) remembers each thread's last request, and
// a crash report names the heap that request went to. Diagnostic only;
// nothing is changed.

struct ElfImage;

void sf_heap_probe_install(ElfImage* image);

// From the SIGSEGV handler, on the faulting thread: the last AllocAutoHeap
// request this thread made and the state of the heap it went to. Nothing
// when the probe is off.
void sf_heap_probe_crash_report();
