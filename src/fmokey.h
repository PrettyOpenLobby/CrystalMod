// fmokey.h -- declarations for the FMO session-key tap.
//
// Deliberately NOT in polshim.h: this module needs nothing from that header,
// and keeping its declarations here keeps the shared header out of the change.
#pragma once

void fmokey_configure(const wchar_t* ini);
void fmokey_apply_module(void* module_base, const char* module);
int  fmokey_enabled();
void fmokey_summary();
void fmokey_stop();      // signal the gate watcher to exit (signal-only, no join)

//: True when either the key tap or the gate watcher wants module callbacks, so
//: inject.cpp can keep using one test.
int  fmokey_wants_modules();   // key tap only now; the watcher self-starts
