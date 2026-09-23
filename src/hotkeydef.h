// The settings-hotkey default, in ONE place.
//
// It is a LIST -- any chord in it opens the dialog. Two defaults, on purpose:
//
//   home           a single key anyone can find without being told, but it is missing
//                  from compact keyboards and the Steam Deck
//   ctrl+shift+s   universal, and utterly forgettable
//
// Shipping both means the discoverable one works where it exists and the universal one
// covers the rest; the user narrows it to whatever they like in the settings dialog
// (`[settings] hotkey`, an OPT_TEXT row, so it is an edit box).
//
// This header exists because this default otherwise lives in THREE places that must
// agree -- iniheal.cpp's option table, polsettings.cpp's fallback, and the shipped ini
// templates. The first two now read it from here; the templates are text and still have
// to be kept in step by hand.
#pragma once

#define POLSHIM_DEFAULT_HOTKEY   L"home,ctrl+shift+s"

// The REPORT key's default, here for the same reason: iniheal.cpp's option table
// and polsettings.cpp's fallback both need it. Deliberately NOT a bare key: `end`
// is what a player presses in a chat box to jump to the end of the line, and a
// report box opening mid-sentence is its own bug. The controller chord,
// `back+rb`, is in the option table beside it.
#define POLREPORT_DEFAULT_HOTKEY L"ctrl+shift+r"
