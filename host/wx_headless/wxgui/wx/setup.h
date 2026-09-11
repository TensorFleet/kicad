/*
 * wx/setup.h for the Emscripten (and any wxBase-only) KiCad build.
 *
 * The wxWidgets in build/wasm-deps/prefix was configured --disable-gui, so its
 * installed setup.h says wxUSE_GUI 0 and its installed header set is the ~170
 * base headers.  KiCad's headless translation units still *name* wxBitmap,
 * wxFont, wxDC, wxWindow, wxColour, wxImage, wxGridTableBase ... and with
 * wxUSE_GUI 0 those classes are not declared at all, so the TUs fail to
 * compile rather than merely to link.
 *
 * This file is the same setup.h with wxUSE_GUI (and the feature flags whose
 * headers KiCad includes) forced on, so that the FULL wx 3.2.11 header set from
 * the source tree can be used.  Nothing extra is linked: every GUI entry point
 * that survives ends up in host/headless_link_stubs.cpp.
 *
 * Layout safety: the only wxUSE_GUI conditionals in the headers of the classes
 * wxBase actually defines (wxAppConsole, wxAppTraits, wxEvtHandler, wxLog,
 * wxMessageOutput) add *separate* derived classes, they never add members or
 * virtuals to a base class.  tools/wasm/check_wx_abi.sh asserts the sizes.
 */
#ifndef KICAD_WX_HEADLESS_SETUP_H
#define KICAD_WX_HEADLESS_SETUP_H

/* Pick a port whose PUBLIC headers name no toolkit header.  wxGTK's do not
 * (only wx/gtk/webview_webkit.h, which KiCad's headless set never includes),
 * and KiCad is built against wxGTK on Linux, so this is the best-tested path. */
#ifndef __WXGTK__
#define __WXGTK__ 1
#endif
#ifndef __WXGTK20__
#define __WXGTK20__ 1
#endif
#ifndef __WXGTK3__
#define __WXGTK3__ 1
#endif

/* setup.h guards this one with #ifndef, so defining it first is enough. */
#define wxUSE_GUI 1

/* wx/log.h defaults this to 0 unless wxDEBUG_LEVEL >= 2, and the --disable-gui setup.h
 * does not pin it.  KiCad's wxLogTrace() calls are unconditional. */
#define wxUSE_LOG_TRACE 1

#include <base-unicode-static-3.2/wx/setup.h>

/* ------------------------------------------------------------------------ */
/* Feature flags the --disable-gui configure turned off.  Declarations only.  */
/* ------------------------------------------------------------------------ */
/* Generated from the installed setup.h: every wxUSE_* the --disable-gui
 * configure left at 0, minus a deny list of things that are genuinely absent
 * under Emscripten (threads, sockets, dlopen, the MSW/GTK/OSX natives, the
 * image codecs) or that would change the ABI of a class wxBase defines
 * (wxUSE_THREADS, wxUSE_STL, wxUSE_UNICODE_UTF8, wxUSE_EXTENDED_RTTI).
 * Declarations only -- the definitions come from host/headless_link_stubs.cpp
 * and host/wx_headless/. */
#undef  wxUSE_LOGWINDOW
#define wxUSE_LOGWINDOW 1
#undef  wxUSE_LOGGUI
#define wxUSE_LOGGUI 1
#undef  wxUSE_LOG_DIALOG
#define wxUSE_LOG_DIALOG 1
#undef  wxUSE_XRC
#define wxUSE_XRC 1
#undef  wxUSE_AUI
#define wxUSE_AUI 1
#undef  wxUSE_PROPGRID
#define wxUSE_PROPGRID 1
#undef  wxUSE_STC
#define wxUSE_STC 1
#undef  wxUSE_GRAPHICS_CONTEXT
#define wxUSE_GRAPHICS_CONTEXT 1
#undef  wxUSE_CONTROLS
#define wxUSE_CONTROLS 1
#undef  wxUSE_MARKUP
#define wxUSE_MARKUP 1
#undef  wxUSE_POPUPWIN
#define wxUSE_POPUPWIN 1
#undef  wxUSE_TIPWINDOW
#define wxUSE_TIPWINDOW 1
#undef  wxUSE_ACTIVITYINDICATOR
#define wxUSE_ACTIVITYINDICATOR 1
#undef  wxUSE_ANIMATIONCTRL
#define wxUSE_ANIMATIONCTRL 1
#undef  wxUSE_BANNERWINDOW
#define wxUSE_BANNERWINDOW 1
#undef  wxUSE_BUTTON
#define wxUSE_BUTTON 1
#undef  wxUSE_BMPBUTTON
#define wxUSE_BMPBUTTON 1
#undef  wxUSE_CALENDARCTRL
#define wxUSE_CALENDARCTRL 1
#undef  wxUSE_CHECKBOX
#define wxUSE_CHECKBOX 1
#undef  wxUSE_CHECKLISTBOX
#define wxUSE_CHECKLISTBOX 1
#undef  wxUSE_CHOICE
#define wxUSE_CHOICE 1
#undef  wxUSE_COLLPANE
#define wxUSE_COLLPANE 1
#undef  wxUSE_COLOURPICKERCTRL
#define wxUSE_COLOURPICKERCTRL 1
#undef  wxUSE_COMBOBOX
#define wxUSE_COMBOBOX 1
#undef  wxUSE_COMMANDLINKBUTTON
#define wxUSE_COMMANDLINKBUTTON 1
#undef  wxUSE_DATAVIEWCTRL
#define wxUSE_DATAVIEWCTRL 1
#undef  wxUSE_DATEPICKCTRL
#define wxUSE_DATEPICKCTRL 1
#undef  wxUSE_DIRPICKERCTRL
#define wxUSE_DIRPICKERCTRL 1
#undef  wxUSE_EDITABLELISTBOX
#define wxUSE_EDITABLELISTBOX 1
#undef  wxUSE_FILECTRL
#define wxUSE_FILECTRL 1
#undef  wxUSE_FILEPICKERCTRL
#define wxUSE_FILEPICKERCTRL 1
#undef  wxUSE_FONTPICKERCTRL
#define wxUSE_FONTPICKERCTRL 1
#undef  wxUSE_GAUGE
#define wxUSE_GAUGE 1
#undef  wxUSE_HEADERCTRL
#define wxUSE_HEADERCTRL 1
#undef  wxUSE_HYPERLINKCTRL
#define wxUSE_HYPERLINKCTRL 1
#undef  wxUSE_LISTBOX
#define wxUSE_LISTBOX 1
#undef  wxUSE_LISTCTRL
#define wxUSE_LISTCTRL 1
#undef  wxUSE_RADIOBOX
#define wxUSE_RADIOBOX 1
#undef  wxUSE_RADIOBTN
#define wxUSE_RADIOBTN 1
#undef  wxUSE_RICHMSGDLG
#define wxUSE_RICHMSGDLG 1
#undef  wxUSE_SCROLLBAR
#define wxUSE_SCROLLBAR 1
#undef  wxUSE_SEARCHCTRL
#define wxUSE_SEARCHCTRL 1
#undef  wxUSE_SLIDER
#define wxUSE_SLIDER 1
#undef  wxUSE_SPINBTN
#define wxUSE_SPINBTN 1
#undef  wxUSE_SPINCTRL
#define wxUSE_SPINCTRL 1
#undef  wxUSE_STATBOX
#define wxUSE_STATBOX 1
#undef  wxUSE_STATLINE
#define wxUSE_STATLINE 1
#undef  wxUSE_STATTEXT
#define wxUSE_STATTEXT 1
#undef  wxUSE_STATBMP
#define wxUSE_STATBMP 1
#undef  wxUSE_TEXTCTRL
#define wxUSE_TEXTCTRL 1
#undef  wxUSE_TIMEPICKCTRL
#define wxUSE_TIMEPICKCTRL 1
#undef  wxUSE_TOGGLEBTN
#define wxUSE_TOGGLEBTN 1
#undef  wxUSE_TREECTRL
#define wxUSE_TREECTRL 1
#undef  wxUSE_TREELISTCTRL
#define wxUSE_TREELISTCTRL 1
#undef  wxUSE_STATUSBAR
#define wxUSE_STATUSBAR 1
#undef  wxUSE_TOOLBAR
#define wxUSE_TOOLBAR 1
#undef  wxUSE_TOOLBAR_NATIVE
#define wxUSE_TOOLBAR_NATIVE 1
#undef  wxUSE_NOTEBOOK
#define wxUSE_NOTEBOOK 1
#undef  wxUSE_LISTBOOK
#define wxUSE_LISTBOOK 1
#undef  wxUSE_CHOICEBOOK
#define wxUSE_CHOICEBOOK 1
#undef  wxUSE_TREEBOOK
#define wxUSE_TREEBOOK 1
#undef  wxUSE_TOOLBOOK
#define wxUSE_TOOLBOOK 1
#undef  wxUSE_GRID
#define wxUSE_GRID 1
#undef  wxUSE_MINIFRAME
#define wxUSE_MINIFRAME 1
#undef  wxUSE_COMBOCTRL
#define wxUSE_COMBOCTRL 1
#undef  wxUSE_ODCOMBOBOX
#define wxUSE_ODCOMBOBOX 1
#undef  wxUSE_BITMAPCOMBOBOX
#define wxUSE_BITMAPCOMBOBOX 1
#undef  wxUSE_REARRANGECTRL
#define wxUSE_REARRANGECTRL 1
#undef  wxUSE_ADDREMOVECTRL
#define wxUSE_ADDREMOVECTRL 1
#undef  wxUSE_ACCEL
#define wxUSE_ACCEL 1
#undef  wxUSE_ARTPROVIDER_STD
#define wxUSE_ARTPROVIDER_STD 1
#undef  wxUSE_CARET
#define wxUSE_CARET 1
#undef  wxUSE_DISPLAY
#define wxUSE_DISPLAY 1
#undef  wxUSE_IMAGLIST
#define wxUSE_IMAGLIST 1
#undef  wxUSE_INFOBAR
#define wxUSE_INFOBAR 1
#undef  wxUSE_MENUS
#define wxUSE_MENUS 1
#undef  wxUSE_MENUBAR
#define wxUSE_MENUBAR 1
#undef  wxUSE_PREFERENCES_EDITOR
#define wxUSE_PREFERENCES_EDITOR 1
#undef  wxUSE_PRIVATE_FONTS
#define wxUSE_PRIVATE_FONTS 1
#undef  wxUSE_RICHTOOLTIP
#define wxUSE_RICHTOOLTIP 1
#undef  wxUSE_SASH
#define wxUSE_SASH 1
#undef  wxUSE_SPLITTER
#define wxUSE_SPLITTER 1
#undef  wxUSE_TOOLTIPS
#define wxUSE_TOOLTIPS 1
#undef  wxUSE_VALIDATORS
#define wxUSE_VALIDATORS 1
#undef  wxUSE_COMMON_DIALOGS
#define wxUSE_COMMON_DIALOGS 1
#undef  wxUSE_BUSYINFO
#define wxUSE_BUSYINFO 1
#undef  wxUSE_CHOICEDLG
#define wxUSE_CHOICEDLG 1
#undef  wxUSE_COLOURDLG
#define wxUSE_COLOURDLG 1
#undef  wxUSE_DIRDLG
#define wxUSE_DIRDLG 1
#undef  wxUSE_FILEDLG
#define wxUSE_FILEDLG 1
#undef  wxUSE_FINDREPLDLG
#define wxUSE_FINDREPLDLG 1
#undef  wxUSE_FONTDLG
#define wxUSE_FONTDLG 1
#undef  wxUSE_MSGDLG
#define wxUSE_MSGDLG 1
#undef  wxUSE_PROGRESSDLG
#define wxUSE_PROGRESSDLG 1
#undef  wxUSE_STARTUP_TIPS
#define wxUSE_STARTUP_TIPS 1
#undef  wxUSE_TEXTDLG
#define wxUSE_TEXTDLG 1
#undef  wxUSE_NUMBERDLG
#define wxUSE_NUMBERDLG 1
#undef  wxUSE_SPLASH
#define wxUSE_SPLASH 1
#undef  wxUSE_WIZARDDLG
#define wxUSE_WIZARDDLG 1
#undef  wxUSE_ABOUTDLG
#define wxUSE_ABOUTDLG 1
#undef  wxUSE_METAFILE
#define wxUSE_METAFILE 1
#undef  wxUSE_DOC_VIEW_ARCHITECTURE
#define wxUSE_DOC_VIEW_ARCHITECTURE 1
#undef  wxUSE_PRINTING_ARCHITECTURE
#define wxUSE_PRINTING_ARCHITECTURE 1
#undef  wxUSE_HTML
#define wxUSE_HTML 1
#undef  wxUSE_RICHTEXT
#define wxUSE_RICHTEXT 1
#undef  wxUSE_CLIPBOARD
#define wxUSE_CLIPBOARD 1
#undef  wxUSE_DRAG_AND_DROP
#define wxUSE_DRAG_AND_DROP 1
#undef  wxUSE_DRAGIMAGE
#define wxUSE_DRAGIMAGE 1
#undef  wxUSE_CONSTRAINTS
#define wxUSE_CONSTRAINTS 1
#undef  wxUSE_SPLINES
#define wxUSE_SPLINES 1
#undef  wxUSE_MOUSEWHEEL
#define wxUSE_MOUSEWHEEL 1
#undef  wxUSE_POSTSCRIPT
#define wxUSE_POSTSCRIPT 1
#undef  wxUSE_AFM_FOR_POSTSCRIPT
#define wxUSE_AFM_FOR_POSTSCRIPT 1
#undef  wxUSE_DC_TRANSFORM_MATRIX
#define wxUSE_DC_TRANSFORM_MATRIX 1
#undef  wxUSE_IMAGE
#define wxUSE_IMAGE 1
#undef  wxUSE_PALETTE
#define wxUSE_PALETTE 1
#undef  wxUSE_DC_CACHEING
#define wxUSE_DC_CACHEING 1
#undef  wxUSE_OWNER_DRAWN
#define wxUSE_OWNER_DRAWN 1

/* No image codecs are linked; wx's own handlers must not be declared as if
 * they were, or wxInitAllImageHandlers would pull them in. */
#undef  wxUSE_LIBPNG
#define wxUSE_LIBPNG 0
#undef  wxUSE_LIBJPEG
#define wxUSE_LIBJPEG 0
#undef  wxUSE_LIBTIFF
#define wxUSE_LIBTIFF 0

/* chkconf.h rejects these two without wxUSE_LIBPNG: wxSVGFileDC writes its
 * bitmaps as PNG, and the Tango art provider is a PNG blob. */
/* wxWindowBase::GetHelpTextAtPoint() is declared only under wxUSE_HELP, and
 * include/widgets/resettable_panel.h overrides it. */
#undef  wxUSE_HELP
#define wxUSE_HELP 1

#undef  wxUSE_SVG
#define wxUSE_SVG 0
#undef  wxUSE_ARTPROVIDER_TANGO
#define wxUSE_ARTPROVIDER_TANGO 0

#endif /* KICAD_WX_HEADLESS_SETUP_H */
