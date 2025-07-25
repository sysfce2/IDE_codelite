//////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////
//
// copyright            : (C) 2008 by Eran Ifrah
// file name            : cl_editor.cpp
//
// -------------------------------------------------------------------------
// A
//              _____           _      _     _ _
//             /  __ \         | |    | |   (_) |
//             | /  \/ ___   __| | ___| |    _| |_ ___
//             | |    / _ \ / _  |/ _ \ |   | | __/ _ )
//             | \__/\ (_) | (_| |  __/ |___| | ||  __/
//              \____/\___/ \__,_|\___\_____/_|\__\___|
//
//                                                  F i l e
//
//    This program is free software; you can redistribute it and/or modify
//    it under the terms of the GNU General Public License as published by
//    the Free Software Foundation; either version 2 of the License, or
//    (at your option) any later version.
//
//////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////

#include "cl_editor.h"

#include "BreakpointsView.hpp"
#include "ColoursAndFontsManager.h"
#include "CompletionHelper.hpp"
#include "Debugger/debuggersettings.h"
#include "StringUtils.h"
#include "attribute_style.h"
#include "bitmap_loader.h"
#include "bookmark_manager.h"
#include "buildtabsettingsdata.h"
#include "cc_box_tip_window.h"
#include "clEditorStateLocker.h"
#include "clIdleEventThrottler.hpp"
#include "clPrintout.h"
#include "clResizableTooltip.h"
#include "clSFTPManager.hpp"
#include "clSTCHelper.hpp"
#include "clSTCLineKeeper.h"
#include "clWorkspaceManager.h"
#include "cl_command_event.h"
#include "cl_editor_tip_window.h"
#include "codelite_events.h"
#include "context_manager.h"
#include "ctags_manager.h"
#include "debuggerconfigtool.h"
#include "debuggerpane.h"
#include "drawingutils.h"
#include "editor_config.h"
#include "event_notifier.h"
#include "file_logger.h"
#include "fileutils.h"
#include "findresultstab.h"
#include "frame.h"
#include "globals.h"
#include "imanager.h"
#include "lexer_configuration.h"
#include "localworkspace.h"
#include "macromanager.h"
#include "manager.h"
#include "menumanager.h"
#include "new_quick_watch_dlg.h"
#include "pluginmanager.h"
#include "quickdebuginfo.h"
#include "quickfindbar.h"
#include "simpletable.h"
#include "stringhighlighterjob.h"
#include "stringsearcher.h"
#include "tags_options_data.h"
#include "wxCodeCompletionBoxManager.h"

#include <algorithm>
#include <wx/dataobj.h>
#include <wx/display.h>
#include <wx/filedlg.h>
#include <wx/filefn.h>
#include <wx/filename.h>
#include <wx/fontmap.h>
#include <wx/log.h>
#include <wx/msgdlg.h>
#include <wx/printdlg.h>
#include <wx/regex.h>
#include <wx/richtooltip.h> // wxRichToolTip
#include <wx/stc/stc.h>
#include <wx/textdlg.h>
#include <wx/wupdlock.h>
#include <wx/wxcrt.h>

// #include "clFileOrFolderDropTarget.h"

#if wxUSE_PRINTING_ARCHITECTURE
#include <wx/paper.h>
#endif // wxUSE_PRINTING_ARCHITECTURE

#if defined(USE_UCHARDET)
#include "uchardet/uchardet.h"
#endif

#define CL_LINE_MODIFIED_STYLE 200
#define CL_LINE_SAVED_STYLE 201

// debugger line marker xpms
extern const char* arrow_right_green_xpm[];
extern const char* stop_xpm[]; // Breakpoint
extern const char* BreakptDisabled[];
extern const char* BreakptCommandList[];
extern const char* BreakptCommandListDisabled[];
extern const char* BreakptIgnore[];
extern const char* ConditionalBreakpt[];
extern const char* ConditionalBreakptDisabled[];

wxDEFINE_EVENT(wxCMD_EVENT_REMOVE_MATCH_INDICATOR, wxCommandEvent);
wxDEFINE_EVENT(wxCMD_EVENT_ENABLE_WORD_HIGHLIGHT, wxCommandEvent);

// Instantiate statics
std::map<wxString, int> clEditor::ms_bookmarkShapes;
bool clEditor::m_ccShowPrivateMembers = true;
bool clEditor::m_ccShowItemsComments = true;
bool clEditor::m_ccInitialized = false;

// This is needed for wxWidgets < 3.1
#ifndef wxSTC_MARK_BOOKMARK
#define wxSTC_MARK_BOOKMARK wxSTC_MARK_LEFTRECT
#endif

wxPrintData* g_printData = NULL;
wxPageSetupDialogData* g_pageSetupData = NULL;

//---------------------------------------------------------------------------------------
//---------------------------------------------------------------------------------------
namespace
{

int ID_OPEN_URL = wxNOT_FOUND;

// Margins. The orders here matters
constexpr int FOLD_MARGIN_ID = 0;
constexpr int NUMBER_MARGIN_ID = 1;
constexpr int EDIT_TRACKER_MARGIN_ID = 2;
constexpr int SYMBOLS_MARGIN_ID = 3;
constexpr int SYMBOLS_MARGIN_SEP_ID = 4;
constexpr int LAST_MARGIN_ID = 4;
constexpr int MARGIN_WIDTH = 16;

/// A helper class that sets the cursor of the current control to
/// left pointing arrow and restores it once its destroyed
struct CursorChanger {
    wxWindow* win = nullptr;
    wxCursor old_cursor;
    CursorChanger(wxWindow* w)
        : win(w)
    {
        CHECK_PTR_RET(win);
        old_cursor = win->GetCursor();
        win->SetCursor(wxCURSOR_ARROW);
    }

    ~CursorChanger()
    {
        CHECK_PTR_RET(win);
        win->SetCursor(old_cursor);
    }
};

class clEditorDropTarget : public wxDropTarget
{
    wxStyledTextCtrl* m_stc;

public:
    clEditorDropTarget(wxStyledTextCtrl* stc)
        : m_stc(stc)
    {
        wxDataObjectComposite* dataobj = new wxDataObjectComposite();
        dataobj->Add(new wxTextDataObject(), true);
        dataobj->Add(new wxFileDataObject());
        SetDataObject(dataobj);
    }

    /**
     * @brief do the actual drop action
     * we support both text and file names
     */
    wxDragResult OnData(wxCoord x, wxCoord y, wxDragResult defaultDragResult)
    {
        if (!GetData()) {
            return wxDragError;
        }
        wxDataObjectComposite* dataobjComp = static_cast<wxDataObjectComposite*>(GetDataObject());
        if (!dataobjComp)
            return wxDragError;

        wxDataFormat format = dataobjComp->GetReceivedFormat();
        wxDataObject* dataobj = dataobjComp->GetObject(format);
        switch (format.GetType()) {
        case wxDF_FILENAME: {
            wxFileDataObject* fileNameObj = static_cast<wxFileDataObject*>(dataobj);
            DoFilesDrop(fileNameObj->GetFilenames());
        } break;
        case wxDF_UNICODETEXT: {
            wxTextDataObject* textObj = static_cast<wxTextDataObject*>(dataobj);
            wxString text = textObj->GetText();
#ifdef __WXOSX__
            // On OSX, textObj->GetText() returns some garbeled text
            // so use the editor to get the text that we want to copy/move
            text = m_stc->GetSelectedText();
#endif
            if (!DoTextDrop(text, x, y, (defaultDragResult == wxDragMove))) {
                return wxDragCancel;
            }
        } break;
        default:
            break;
        }
        return defaultDragResult;
    }

    /**
     * @brief open list of files in the editor
     */
    bool DoTextDrop(const wxString& text, wxCoord x, wxCoord y, bool moving)
    {
        // insert the text
        int pos = m_stc->PositionFromPoint(wxPoint(x, y));
        if (pos == wxNOT_FOUND)
            return false;

        // Don't allow dropping tabs on the editor
        static wxRegEx re("\\{Class:Notebook,TabIndex:([0-9]+)\\}\\{.*?\\}", wxRE_ADVANCED);
        if (re.Matches(text))
            return false;

        int selStart = m_stc->GetSelectionStart();
        int selEnd = m_stc->GetSelectionEnd();

        // No text dnd if the drop is on the selection
        if ((pos >= selStart) && (pos <= selEnd))
            return false;
        int length = (selEnd - selStart);

        m_stc->BeginUndoAction();
        if (moving) {
            // Clear the selection

            bool movingForward = (pos > selEnd);
            m_stc->InsertText(pos, text);
            if (movingForward) {
                m_stc->Replace(selStart, selEnd, "");
                pos -= length;
            } else {
                m_stc->Replace(selStart + length, selEnd + length, "");
            }
            m_stc->SetSelectionStart(pos);
            m_stc->SetSelectionEnd(pos);
            m_stc->SetCurrentPos(pos);
        } else {
            m_stc->SelectNone();
            m_stc->SetSelectionStart(pos);
            m_stc->SetSelectionEnd(pos);
            m_stc->InsertText(pos, text);
            m_stc->SetCurrentPos(pos);
        }
        m_stc->EndUndoAction();
#ifndef __WXOSX__
        m_stc->CallAfter(&wxStyledTextCtrl::SetSelection, pos, pos + length);
#endif
        return true;
    }

    /**
     * @brief open list of files in the editor
     */
    void DoFilesDrop(const wxArrayString& filenames)
    {
        // Split the list into 2: files and folders
        wxArrayString files, folders;
        for (size_t i = 0; i < filenames.size(); ++i) {
            if (wxFileName::DirExists(filenames.Item(i))) {
                folders.Add(filenames.Item(i));
            } else {
                files.Add(filenames.Item(i));
            }
        }

        for (size_t i = 0; i < files.size(); ++i) {
            clMainFrame::Get()->GetMainBook()->OpenFile(files.Item(i));
        }
    }

    bool OnDrop(wxCoord x, wxCoord y) { return true; }
    wxDragResult OnDragOver(wxCoord x, wxCoord y, wxDragResult defResult) { return m_stc->DoDragOver(x, y, defResult); }
};

bool IsWordChar(const wxChar& ch)
{
    static wxStringSet_t wordsChar;
    if (wordsChar.empty()) {
        wxString chars = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ_.>";
        for (size_t i = 0; i < chars.size(); ++i) {
            wordsChar.insert(chars[i]);
        }
    }
    return (wordsChar.count(ch) != 0);
}

void scroll_range(wxStyledTextCtrl* ctrl, int selection_start, int selection_end)
{
#if wxCHECK_VERSION(3, 1, 0)
    // ensure the selection is visible
    if (selection_end != selection_start) {
        ctrl->ScrollRange(selection_start, selection_end);
    }
#else
    // implement a wx30 version for ScrollRange()
    wxUnusedVar(selection_start);
    wxUnusedVar(selection_end);
#endif
    ctrl->EnsureCaretVisible(); // incase we are inside a folded area
}

#if defined(__WXMSW__)
bool MSWRemoveROFileAttribute(const wxFileName& fileName)
{
    DWORD dwAttrs = GetFileAttributes(fileName.GetFullPath().c_str());
    if (dwAttrs != INVALID_FILE_ATTRIBUTES) {
        if (dwAttrs & FILE_ATTRIBUTE_READONLY) {
            if (wxMessageBox(wxString::Format(wxT("'%s' \n%s\n%s"),
                                              fileName.GetFullPath(),
                                              _("has the read-only attribute set"),
                                              _("Would you like CodeLite to try and remove it?")),
                             _("CodeLite"),
                             wxYES_NO | wxICON_QUESTION | wxCENTER) == wxYES) {
                // try to clear the read-only flag from the file
                if (SetFileAttributes(fileName.GetFullPath().c_str(), dwAttrs & ~(FILE_ATTRIBUTE_READONLY)) == FALSE) {
                    wxMessageBox(wxString::Format(wxT("%s '%s' %s"),
                                                  _("Failed to open file"),
                                                  fileName.GetFullPath().c_str(),
                                                  _("for write")),
                                 _("CodeLite"),
                                 wxOK | wxCENTER | wxICON_WARNING);
                    return false;
                }
            } else {
                return false;
            }
        }
    }
    return true;
}
#endif

constexpr int STYLE_CURRENT_LINE = (wxSTC_STYLE_MAX - 1);
constexpr int STYLE_NORMAL_LINE = (wxSTC_STYLE_MAX - 2);
constexpr int STYLE_MODIFIED_LINE = (wxSTC_STYLE_MAX - 3);
constexpr int STYLE_SAVED_LINE = (wxSTC_STYLE_MAX - 4);
constexpr int STYLE_CURRENT_LINE_MODIFIED = (wxSTC_STYLE_MAX - 5);
constexpr int STYLE_CURRENT_LINE_SAVED = (wxSTC_STYLE_MAX - 6);

wxColour GetContrastColour(const wxColour& c)
{
    if (DrawingUtils::IsDark(c)) {
        return c.ChangeLightness(180);
    } else {
        return c.ChangeLightness(20);
    }
}

/// return the default FG colour for `ctrl`
wxColour GetDefaultFgColour(wxStyledTextCtrl* ctrl) { return ctrl->StyleGetBackground(0); }

/// return true if the default FG colour for `ctrl` is dark
bool IsDefaultFgColourDark(wxStyledTextCtrl* ctrl) { return DrawingUtils::IsDark(GetDefaultFgColour(ctrl)); }

void SetCurrentLineMarginStyle(wxStyledTextCtrl* ctrl)
{
    // Use a distinct style to highlight the current line number
    wxColour default_bg_colour = ctrl->StyleGetBackground(wxSTC_STYLE_LINENUMBER);
    wxColour default_fg_colour = DrawingUtils::IsDark(default_bg_colour) ? default_bg_colour.ChangeLightness(120)
                                                                         : default_bg_colour.ChangeLightness(80);
    wxColour current_line_bg_colour = ctrl->StyleGetBackground(0);

    wxColour RED("RED");
    wxColour ORANGE("GOLD");
    wxColour GREEN("FOREST GREEN");

    bool is_dark = DrawingUtils::IsDark(current_line_bg_colour);
    if (is_dark) {
        current_line_bg_colour = current_line_bg_colour.ChangeLightness(110);
    } else {
        current_line_bg_colour = current_line_bg_colour.ChangeLightness(95);
    }
    wxColour MODIFIED_COLOUR = is_dark ? ORANGE : RED;

    ctrl->StyleSetForeground(STYLE_CURRENT_LINE, GetContrastColour(current_line_bg_colour));
    ctrl->StyleSetBackground(STYLE_CURRENT_LINE, current_line_bg_colour);

    ctrl->StyleSetForeground(STYLE_CURRENT_LINE_MODIFIED, GetContrastColour(MODIFIED_COLOUR));
    ctrl->StyleSetBackground(STYLE_CURRENT_LINE_MODIFIED, MODIFIED_COLOUR);

    ctrl->StyleSetForeground(STYLE_CURRENT_LINE_SAVED, GetContrastColour(GREEN));
    ctrl->StyleSetBackground(STYLE_CURRENT_LINE_SAVED, GREEN);

    ctrl->StyleSetForeground(STYLE_NORMAL_LINE, default_fg_colour);
    ctrl->StyleSetBackground(STYLE_NORMAL_LINE, default_bg_colour);

    ctrl->StyleSetForeground(STYLE_MODIFIED_LINE, is_dark ? ORANGE : RED);
    ctrl->StyleSetBackground(STYLE_MODIFIED_LINE, default_bg_colour);

    ctrl->StyleSetForeground(STYLE_SAVED_LINE, GREEN);
    ctrl->StyleSetBackground(STYLE_SAVED_LINE, default_bg_colour);
}

void GetLineMarginColours(wxStyledTextCtrl* ctrl, wxColour* bg_colour, wxColour* fg_colour)
{
    // Use a distinct style to highlight the current line number
    *bg_colour = ctrl->StyleGetBackground(0);
    *fg_colour = *bg_colour;
    if (DrawingUtils::IsDark(*bg_colour)) {
        *fg_colour = bg_colour->ChangeLightness(125);
    } else {
        *fg_colour = bg_colour->ChangeLightness(70);
    }
}

/// Check to see if we have a .clang-format file in the workspace folder. If we do, read the
/// IndentWidth property

int GetWorkspaceIndentWidth()
{
    if (!clWorkspaceManager::Get().IsWorkspaceOpened()) {
        return wxNOT_FOUND;
    }
    return clWorkspaceManager::Get().GetWorkspace()->GetIndentWidth();
}
} // namespace

//=====================================================================
clEditor::clEditor(wxWindow* parent)
    : m_popupIsOn(false)
    , m_isDragging(false)
    , m_modifyTime(0)
    , m_modificationCount(0)
    , m_isVisible(true)
    , m_hyperLinkIndicatroStart(wxNOT_FOUND)
    , m_hyperLinkIndicatroEnd(wxNOT_FOUND)
    , m_hightlightMatchedBraces(true)
    , m_autoAddMatchedCurlyBrace(false)
    , m_autoAddNormalBraces(false)
    , m_autoAdjustHScrollbarWidth(true)
    , m_reloadingFile(false)
    , m_functionTip(NULL)
    , m_calltip(NULL)
    , m_lastCharEntered(0)
    , m_lastCharEnteredPos(0)
    , m_isFocused(true)
    , m_findBookmarksActive(false)
    , m_mgr(PluginManager::Get())
    , m_richTooltip(NULL)
    , m_lastEndLine(0)
    , m_lastLineCount(0)
{
#if !CL_USE_NATIVEBOOK
    Hide();
#endif

    wxStyledTextCtrl::Create(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxNO_BORDER);
    MSWSetWindowDarkTheme(this);

    Bind(wxEVT_IDLE, &clEditor::OnIdle, this);
    Bind(wxEVT_STC_CHARADDED, &clEditor::OnCharAdded, this);
    Bind(wxEVT_STC_MARGINCLICK, &clEditor::OnMarginClick, this);
    Bind(wxEVT_STC_CALLTIP_CLICK, &clEditor::OnCallTipClick, this);
    Bind(wxEVT_STC_DWELLEND, &clEditor::OnDwellEnd, this);
    Bind(wxEVT_STC_START_DRAG, &clEditor::OnDragStart, this);
    Bind(wxEVT_STC_DO_DROP, &clEditor::OnDragEnd, this);
    Bind(wxEVT_STC_UPDATEUI, &clEditor::OnSciUpdateUI, this);
    Bind(wxEVT_STC_SAVEPOINTREACHED, &clEditor::OnSavePoint, this);
    Bind(wxEVT_STC_SAVEPOINTLEFT, &clEditor::OnSavePoint, this);
    Bind(wxEVT_STC_MODIFIED, &clEditor::OnChange, this);
    Bind(wxEVT_CONTEXT_MENU, &clEditor::OnContextMenu, this);
    Bind(wxEVT_KEY_DOWN, &clEditor::OnKeyDown, this);
    Bind(wxEVT_KEY_UP, &clEditor::OnKeyUp, this);
    Bind(wxEVT_LEFT_DOWN, &clEditor::OnLeftDown, this);
    Bind(wxEVT_RIGHT_DOWN, &clEditor::OnRightDown, this);
    Bind(wxEVT_MOTION, &clEditor::OnMotion, this);
    Bind(wxEVT_MOUSEWHEEL, &clEditor::OnMouseWheel, this);
    Bind(wxEVT_LEFT_UP, &clEditor::OnLeftUp, this);
    Bind(wxEVT_LEAVE_WINDOW, &clEditor::OnLeaveWindow, this);
    Bind(wxEVT_KILL_FOCUS, &clEditor::OnFocusLost, this);
    Bind(wxEVT_SET_FOCUS, &clEditor::OnFocus, this);
    Bind(wxEVT_STC_DOUBLECLICK, &clEditor::OnLeftDClick, this);
    Bind(wxCMD_EVENT_REMOVE_MATCH_INDICATOR, &clEditor::OnRemoveMatchInidicator, this);

    Bind(wxEVT_STC_ZOOM, &clEditor::OnZoom, this);
    UpdateOptions();
    PreferencesChanged();
    EventNotifier::Get()->Bind(wxEVT_EDITOR_CONFIG_CHANGED, &clEditor::OnEditorConfigChanged, this);
    EventNotifier::Get()->Bind(wxEVT_FILE_MODIFIED_EXTERNALLY, &clEditor::OnModifiedExternally, this);
    m_commandsProcessor.SetParent(this);

    SetDropTarget(new clEditorDropTarget(this));

    // User timer to check if we need to highlight markers
    m_timerHighlightMarkers = new wxTimer(this);
    m_timerHighlightMarkers->Start(100, true);

    Connect(m_timerHighlightMarkers->GetId(), wxEVT_TIMER, wxTimerEventHandler(clEditor::OnTimer), NULL, this);

    ms_bookmarkShapes[wxT("Small Rectangle")] = wxSTC_MARK_SMALLRECT;
    ms_bookmarkShapes[wxT("Rounded Rectangle")] = wxSTC_MARK_ROUNDRECT;
    ms_bookmarkShapes[wxT("Small Arrow")] = wxSTC_MARK_ARROW;
    ms_bookmarkShapes[wxT("Circle")] = wxSTC_MARK_CIRCLE;
    ms_bookmarkShapes[wxT("Bookmark")] = wxSTC_MARK_BOOKMARK;

    SetSyntaxHighlight();
    CmdKeyClear(wxT('D'), wxSTC_KEYMOD_CTRL); // clear Ctrl+D because we use it for something else
    Connect(wxEVT_STC_DWELLSTART, wxStyledTextEventHandler(clEditor::OnDwellStart), NULL, this);

    // Initialise the breakpt-marker array
    FillBPtoMarkerArray();

    // set EOL mode for the newly created file
    int eol = GetEOLByOS();
    int alternate_eol = GetEOLByContent();
    if (alternate_eol != wxNOT_FOUND) {
        eol = alternate_eol;
    }
    SetEOLMode(eol);

    // Create the various tip windows
    m_functionTip = new clEditorTipWindow(this);
    m_disableSmartIndent = GetOptions()->GetDisableSmartIndent();

    m_deltas = new EditorDeltasHolder;
    EventNotifier::Get()->Connect(
        wxCMD_EVENT_ENABLE_WORD_HIGHLIGHT, wxCommandEventHandler(clEditor::OnHighlightWordChecked), NULL, this);
    EventNotifier::Get()->Connect(
        wxEVT_CODEFORMATTER_INDENT_STARTING, wxCommandEventHandler(clEditor::OnFileFormatStarting), NULL, this);
    EventNotifier::Get()->Connect(
        wxEVT_CODEFORMATTER_INDENT_COMPLETED, wxCommandEventHandler(clEditor::OnFileFormatDone), NULL, this);
    EventNotifier::Get()->Bind(wxEVT_CMD_COLOURS_FONTS_UPDATED, &clEditor::OnColoursAndFontsUpdated, this);
    EventNotifier::Get()->Bind(wxEVT_ACTIVE_EDITOR_CHANGED, &clEditor::OnActiveEditorChanged, this);
    Bind(wxEVT_COMMAND_MENU_SELECTED,
         wxCommandEventHandler(clEditor::OnChangeActiveBookmarkType),
         this,
         XRCID("BookmarkTypes[start]"),
         XRCID("BookmarkTypes[end]"));

    // Notify that this instance is being instantiated
    clCommandEvent initEvent(wxEVT_EDITOR_INITIALIZING);
    initEvent.SetEventObject(this);
    EventNotifier::Get()->ProcessEvent(initEvent);
}

clEditor::~clEditor()
{
    // Report file-close event
    if (GetFileName().IsOk() && GetFileName().FileExists()) {
        clCommandEvent eventClose(wxEVT_FILE_CLOSED);
        eventClose.SetFileName(FileUtils::RealPath(GetFileName().GetFullPath()));
        EventNotifier::Get()->AddPendingEvent(eventClose);
    }
    wxDELETE(m_richTooltip);
    EventNotifier::Get()->Unbind(wxEVT_ACTIVE_EDITOR_CHANGED, &clEditor::OnActiveEditorChanged, this);
    EventNotifier::Get()->Unbind(wxEVT_EDITOR_CONFIG_CHANGED, &clEditor::OnEditorConfigChanged, this);
    EventNotifier::Get()->Unbind(wxEVT_FILE_MODIFIED_EXTERNALLY, &clEditor::OnModifiedExternally, this);

    EventNotifier::Get()->Disconnect(
        wxCMD_EVENT_ENABLE_WORD_HIGHLIGHT, wxCommandEventHandler(clEditor::OnHighlightWordChecked), NULL, this);
    EventNotifier::Get()->Disconnect(
        wxEVT_CODEFORMATTER_INDENT_STARTING, wxCommandEventHandler(clEditor::OnFileFormatStarting), NULL, this);
    EventNotifier::Get()->Disconnect(
        wxEVT_CODEFORMATTER_INDENT_COMPLETED, wxCommandEventHandler(clEditor::OnFileFormatDone), NULL, this);
    EventNotifier::Get()->Unbind(wxEVT_CMD_COLOURS_FONTS_UPDATED, &clEditor::OnColoursAndFontsUpdated, this);
    Unbind(wxEVT_COMMAND_MENU_SELECTED,
           wxCommandEventHandler(clEditor::OnChangeActiveBookmarkType),
           this,
           XRCID("BookmarkTypes[start]"),
           XRCID("BookmarkTypes[end]"));

    // free the timer
    Disconnect(m_timerHighlightMarkers->GetId(), wxEVT_TIMER, wxTimerEventHandler(clEditor::OnTimer), NULL, this);
    m_timerHighlightMarkers->Stop();
    wxDELETE(m_timerHighlightMarkers);

    // find deltas
    wxDELETE(m_deltas);

    if (this->HasCapture()) {
        this->ReleaseMouse();
    }
}

time_t clEditor::GetFileLastModifiedTime() const
{
    return FileUtils::GetFileModificationTime(m_fileName);
}

void clEditor::SetSyntaxHighlight(const wxString& lexerName)
{
    ClearDocumentStyle();
    m_context = ContextManager::Get()->NewContext(this, lexerName);

    // Apply the lexer fonts and colours before we call
    // "SetProperties". (SetProperties function needs the correct font for
    // some of its settings)
    LexerConf::Ptr_t lexer = ColoursAndFontsManager::Get().GetLexer(lexerName);
    if (lexer) {
        lexer->Apply(this, true);
    }
    CallAfter(&clEditor::SetProperties);

    SetEOL();
    m_context->SetActive();
    m_context->ApplySettings();

    SetCurrentLineMarginStyle(GetCtrl());
    CallAfter(&clEditor::UpdateColours);
}

void clEditor::SetSyntaxHighlight(bool bUpdateColors)
{
    ClearDocumentStyle();
    m_context = ContextManager::Get()->NewContextByFileName(this, m_fileName);

    CallAfter(&clEditor::SetProperties);

    m_context->SetActive();
    m_context->ApplySettings();
    if (bUpdateColors) {
        UpdateColours();
    }
    SetCurrentLineMarginStyle(GetCtrl());
}

// Fills the struct array that marries breakpoint type to marker and mask
void clEditor::FillBPtoMarkerArray()
{
    BPtoMarker bpm;
    bpm.bp_type = BP_type_break;
    bpm.marker = smt_breakpoint;
    bpm.mask = mmt_breakpoint;
    bpm.marker_disabled = smt_bp_disabled;
    bpm.mask_disabled = mmt_bp_disabled;
    m_BPstoMarkers.push_back(bpm);

    BPtoMarker bpcmdm;
    bpcmdm.bp_type = BP_type_cmdlistbreak;
    bpcmdm.marker = smt_bp_cmdlist;
    bpcmdm.mask = mmt_bp_cmdlist;
    bpcmdm.marker_disabled = smt_bp_cmdlist_disabled;
    bpcmdm.mask_disabled = mmt_bp_cmdlist_disabled;
    m_BPstoMarkers.push_back(bpcmdm);

    BPtoMarker bpcondm;
    bpcondm.bp_type = BP_type_condbreak;
    bpcondm.marker = smt_cond_bp;
    bpcondm.mask = mmt_cond_bp;
    bpcondm.marker_disabled = smt_cond_bp_disabled;
    bpcondm.mask_disabled = mmt_cond_bp_disabled;
    m_BPstoMarkers.push_back(bpcondm);

    BPtoMarker bpignm;
    bpignm.bp_type = BP_type_ignoredbreak;
    bpignm.marker = bpignm.marker_disabled = smt_bp_ignored;
    bpignm.mask = bpignm.mask_disabled = mmt_bp_ignored; // Enabled/disabled are the same
    m_BPstoMarkers.push_back(bpignm);

    bpm.bp_type = BP_type_tempbreak;
    m_BPstoMarkers.push_back(bpm); // Temp is the same as non-temp
}

// Looks for a struct for this breakpoint-type
BPtoMarker clEditor::GetMarkerForBreakpt(enum BreakpointType bp_type)
{
    std::vector<BPtoMarker>::iterator iter = m_BPstoMarkers.begin();
    for (; iter != m_BPstoMarkers.end(); ++iter) {
        if ((*iter).bp_type == bp_type) {
            return *iter;
        }
    }
    clLogMessage(wxT("Breakpoint type not in vector!?"));
    return *iter;
}

void clEditor::SetCaretAt(long pos) { clSTCHelper::SetCaretAt(this, pos); }

/// Setup some scintilla properties
void clEditor::SetProperties()
{
#ifndef __WXMSW__
    UsePopUp(false);
#else
    UsePopUp(0);
#endif

    m_lastEndLine = wxNOT_FOUND;
    m_editorState = {};
    m_lastLineCount = 0;

    SetRectangularSelectionModifier(wxSTC_KEYMOD_CTRL);
    SetAdditionalSelectionTyping(true);
    OptionsConfigPtr options = GetOptions();
    CallTipUseStyle(1);
    int lineSpacing = clConfig::Get().Read("extra_line_spacing", (int)0);
    SetExtraAscent(lineSpacing);
    SetExtraDescent(lineSpacing);
    CallTipSetBackground(wxSystemSettings::GetColour(wxSYS_COLOUR_INFOBK));
    CallTipSetForeground(wxSystemSettings::GetColour(wxSYS_COLOUR_INFOTEXT));
    MarkerEnableHighlight(options->IsHighlightFoldWhenActive());

    m_hightlightMatchedBraces = options->GetHighlightMatchedBraces();
    m_autoAddMatchedCurlyBrace = options->GetAutoAddMatchedCurlyBraces();
    m_autoAddNormalBraces = options->GetAutoAddMatchedNormalBraces();
    m_smartParen = options->IsSmartParen();
    m_autoAdjustHScrollbarWidth = options->GetAutoAdjustHScrollBarWidth();
    m_disableSmartIndent = options->GetDisableSmartIndent();
    m_disableSemicolonShift = options->GetDisableSemicolonShift();
    SetMultipleSelection(true);
    SetMultiPaste(1);

    if (!m_hightlightMatchedBraces) {
        wxStyledTextCtrl::BraceHighlight(wxSTC_INVALID_POSITION, wxSTC_INVALID_POSITION);
        SetHighlightGuide(0);
    }

    SetVirtualSpaceOptions(options->HasOption(OptionsConfig::Opt_AllowCaretAfterEndOfLine) ? 2 : 1);
    SetCaretStyle(options->HasOption(OptionsConfig::Opt_UseBlockCaret) ? wxSTC_CARETSTYLE_BLOCK
                                                                       : wxSTC_CARETSTYLE_LINE);
    SetWrapMode(options->GetWordWrap() ? wxSTC_WRAP_WORD : wxSTC_WRAP_NONE);
    SetViewWhiteSpace(options->GetShowWhitespaces());
    SetMouseDwellTime(500);
    SetProperty(wxT("fold"), wxT("1"));
    SetProperty(wxT("fold.html"), wxT("1"));
    SetProperty(wxT("fold.comment"), wxT("1"));

    SetProperty(wxT("fold.at.else"), options->GetFoldAtElse() ? wxT("1") : wxT("0"));
    SetProperty(wxT("fold.preprocessor"), options->GetFoldPreprocessor() ? wxT("1") : wxT("0"));
    SetProperty(wxT("fold.compact"), options->GetFoldCompact() ? wxT("1") : wxT("0"));

    // Fold and comments as well
    SetProperty(wxT("fold.comment"), wxT("1"));
    SetProperty("fold.hypertext.comment", "1");
    SetModEventMask(wxSTC_MOD_DELETETEXT | wxSTC_MOD_INSERTTEXT | wxSTC_PERFORMED_UNDO | wxSTC_PERFORMED_REDO |
                    wxSTC_MOD_BEFOREDELETE | wxSTC_MOD_CHANGESTYLE);

    int caretSlop = 1;
    int caretZone = 20;
    int caretStrict = 0;
    int caretEven = 0;
    int caretJumps = 0;

    SetXCaretPolicy(caretStrict | caretSlop | caretEven | caretJumps, caretZone);

    caretSlop = 1;
    caretZone = 1;
    caretStrict = 4;
    caretEven = 8;
    caretJumps = 0;
    SetYCaretPolicy(caretStrict | caretSlop | caretEven | caretJumps, caretZone);

    // Set the caret width
    SetCaretWidth(options->GetCaretWidth());
    SetCaretPeriod(options->GetCaretBlinkPeriod());
    SetMarginLeft(1);

    // Mark current line
    SetCaretLineVisible(options->GetHighlightCaretLine());
#if wxCHECK_VERSION(3, 3, 0)
    if (options->IsHighlightCaretLineWithColour()) {
        SetCaretLineBackground(options->GetCaretLineColour());
        SetCaretLineBackAlpha(options->GetCaretLineAlpha());
        SetCaretLineFrame(0);

    } else {
        bool is_dark = IsDefaultFgColourDark(this);
        SetCaretLineBackground(is_dark ? wxColour("GRAY") : wxColour("LIGHT GRAY"));
        SetCaretLineBackAlpha(wxSTC_ALPHA_NOALPHA);
        SetCaretLineFrame(1);
    }
#else
    SetCaretLineBackground(options->GetCaretLineColour());
    SetCaretLineBackAlpha(options->GetCaretLineAlpha());
#endif

    SetFoldFlags(options->GetUnderlineFoldLine()
                     ? wxSTC_FOLDFLAG_LINEAFTER_CONTRACTED | wxSTC_FOLDFLAG_LINEBEFORE_CONTRACTED
                     : 0);
    SetEndAtLastLine(!options->GetScrollBeyondLastLine());

    //------------------------------------------
    // Margin settings
    //------------------------------------------

    // symbol margin
    SetMarginType(SYMBOLS_MARGIN_ID, wxSTC_MARGIN_SYMBOL);
    SetMarginCursor(SYMBOLS_MARGIN_ID, 8);

    // Line numbers
    if (options->IsLineNumberHighlightCurrent()) {
        SetMarginType(NUMBER_MARGIN_ID, wxSTC_MARGIN_RTEXT);
    } else {
        SetMarginType(NUMBER_MARGIN_ID, wxSTC_MARGIN_NUMBER);
    }

    // line number margin displays every thing but folding, bookmarks and breakpoint
    SetMarginMask(
        NUMBER_MARGIN_ID,
        ~(mmt_folds | mmt_all_bookmarks | mmt_indicator | mmt_compiler | mmt_all_breakpoints | mmt_line_marker));

    // Hide the "Tracker" margin, we use the line numbers instead
    SetMarginType(EDIT_TRACKER_MARGIN_ID, 4);
    SetMarginWidth(EDIT_TRACKER_MARGIN_ID, 0);
    SetMarginMask(EDIT_TRACKER_MARGIN_ID, 0);
    m_trackChanges = GetOptions()->IsTrackChanges();
    if (!m_trackChanges) {
        m_modifiedLines.clear();
    }

    // Separators
    SetMarginType(SYMBOLS_MARGIN_SEP_ID, wxSTC_MARGIN_COLOUR);
    SetMarginMask(SYMBOLS_MARGIN_SEP_ID, 0);
    SetMarginWidth(SYMBOLS_MARGIN_SEP_ID, FromDIP(1));

    wxColour bgColour = StyleGetBackground(0);
    SetMarginBackground(SYMBOLS_MARGIN_SEP_ID,
                        DrawingUtils::IsDark(bgColour) ? bgColour.ChangeLightness(120) : bgColour.ChangeLightness(60));

    // Set margins' width
    SetMarginWidth(SYMBOLS_MARGIN_ID, options->GetDisplayBookmarkMargin() ? FromDIP(MARGIN_WIDTH) : 0); // Symbol margin

    // allow everything except for the folding symbols
    SetMarginMask(SYMBOLS_MARGIN_ID, ~(wxSTC_MASK_FOLDERS));

    // Show number margin according to settings.
    UpdateLineNumberMarginWidth();

    // Mark fold margin & symbols margins as sensitive
    SetMarginSensitive(SYMBOLS_MARGIN_ID, true);

    // Right margin
    SetEdgeMode(options->IsShowRightMarginIndicator() ? wxSTC_EDGE_LINE : wxSTC_EDGE_NONE);
    SetEdgeColumn(options->GetRightMarginColumn());
    wxColour bg_colour = StyleGetBackground(0);
    SetEdgeColour(DrawingUtils::IsDark(bg_colour) ? bg_colour.ChangeLightness(110) : bg_colour.ChangeLightness(80));

    //---------------------------------------------------
    // Fold settings
    //---------------------------------------------------
    SetMarginCursor(FOLD_MARGIN_ID, 8);
    StyleSetBackground(wxSTC_STYLE_FOLDDISPLAYTEXT, StyleGetBackground(wxSTC_STYLE_DEFAULT));
    StyleSetForeground(wxSTC_STYLE_FOLDDISPLAYTEXT, DrawingUtils::IsDark(bg_colour) ? "YELLOW" : "ORANGE");

    // Determine the folding symbols colours
    wxColour foldFgColour = wxColor(0xff, 0xff, 0xff);
    wxColour foldBgColour = wxColor(0x80, 0x80, 0x80);
    LexerConf::Ptr_t lexer = ColoursAndFontsManager::Get().GetLexer(GetContext()->GetName());
    if (lexer) {
        const StyleProperty& sp = lexer->GetProperty(SEL_TEXT_ATTR_ID);
        m_selTextBgColour = sp.GetBgColour();
        m_selTextColour = sp.GetFgColour();
    } else {
        m_selTextBgColour = StyleGetBackground(0);
        m_selTextColour = StyleGetForeground(0);
    }
    MarkerDefine(smt_line_marker, wxSTC_MARK_LEFTRECT, StyleGetForeground(0));

    if (lexer && lexer->IsDark()) {
        const StyleProperty& defaultProperty = lexer->GetProperty(0);
        if (!defaultProperty.IsNull()) {
            foldFgColour = wxColour(defaultProperty.GetBgColour()).ChangeLightness(130);
            foldBgColour = wxColour(defaultProperty.GetBgColour());
        }
    } else if (lexer) {
        const StyleProperty& defaultProperty = lexer->GetProperty(0);
        if (!defaultProperty.IsNull()) {
            foldFgColour = wxColour(defaultProperty.GetBgColour()).ChangeLightness(70);
            foldBgColour = wxColour(defaultProperty.GetBgColour());
        }
    }

    // ===------------------------------------------------------------
    // Folding setup
    // ===------------------------------------------------------------
    SetMarginMask(FOLD_MARGIN_ID, wxSTC_MASK_FOLDERS);
    SetMarginType(FOLD_MARGIN_ID, wxSTC_MARGIN_SYMBOL);
    SetMarginSensitive(FOLD_MARGIN_ID, true);
    SetMarginWidth(FOLD_MARGIN_ID, options->GetDisplayFoldMargin() ? FromDIP(MARGIN_WIDTH) : 0);
    StyleSetBackground(FOLD_MARGIN_ID, StyleGetBackground(wxSTC_STYLE_DEFAULT));

    if (options->GetFoldStyle() == wxT("Flatten Tree Square Headers")) {
        DefineMarker(wxSTC_MARKNUM_FOLDEROPEN, wxSTC_MARK_BOXMINUS, foldFgColour, foldBgColour);
        DefineMarker(wxSTC_MARKNUM_FOLDER, wxSTC_MARK_BOXPLUS, foldFgColour, foldBgColour);
        DefineMarker(wxSTC_MARKNUM_FOLDERSUB, wxSTC_MARK_VLINE, foldFgColour, foldBgColour);
        DefineMarker(wxSTC_MARKNUM_FOLDERTAIL, wxSTC_MARK_LCORNER, foldFgColour, foldBgColour);
        DefineMarker(wxSTC_MARKNUM_FOLDEREND, wxSTC_MARK_BOXPLUSCONNECTED, foldFgColour, foldBgColour);
        DefineMarker(wxSTC_MARKNUM_FOLDEROPENMID, wxSTC_MARK_BOXMINUSCONNECTED, foldFgColour, foldBgColour);
        DefineMarker(wxSTC_MARKNUM_FOLDERMIDTAIL, wxSTC_MARK_TCORNER, foldFgColour, foldBgColour);

    } else if (options->GetFoldStyle() == wxT("Flatten Tree Circular Headers")) {
        DefineMarker(wxSTC_MARKNUM_FOLDEROPEN, wxSTC_MARK_CIRCLEMINUS, foldFgColour, foldBgColour);
        DefineMarker(wxSTC_MARKNUM_FOLDER, wxSTC_MARK_CIRCLEPLUS, foldFgColour, foldBgColour);
        DefineMarker(wxSTC_MARKNUM_FOLDERSUB, wxSTC_MARK_VLINE, foldFgColour, foldBgColour);
        DefineMarker(wxSTC_MARKNUM_FOLDERTAIL, wxSTC_MARK_LCORNERCURVE, foldFgColour, foldBgColour);
        DefineMarker(wxSTC_MARKNUM_FOLDEREND, wxSTC_MARK_CIRCLEPLUSCONNECTED, foldFgColour, foldBgColour);
        DefineMarker(wxSTC_MARKNUM_FOLDEROPENMID, wxSTC_MARK_CIRCLEMINUSCONNECTED, foldFgColour, foldBgColour);
        DefineMarker(wxSTC_MARKNUM_FOLDERMIDTAIL, wxSTC_MARK_TCORNER, foldFgColour, foldBgColour);

    } else if (options->GetFoldStyle() == wxT("Simple")) {
        DefineMarker(wxSTC_MARKNUM_FOLDEROPEN, wxSTC_MARK_MINUS, foldFgColour, foldBgColour);
        DefineMarker(wxSTC_MARKNUM_FOLDER, wxSTC_MARK_PLUS, foldFgColour, foldBgColour);
        DefineMarker(wxSTC_MARKNUM_FOLDERSUB, wxSTC_MARK_BACKGROUND, foldFgColour, foldBgColour);
        DefineMarker(wxSTC_MARKNUM_FOLDERTAIL, wxSTC_MARK_BACKGROUND, foldFgColour, foldBgColour);
        DefineMarker(wxSTC_MARKNUM_FOLDEREND, wxSTC_MARK_PLUS, foldFgColour, foldBgColour);
        DefineMarker(wxSTC_MARKNUM_FOLDEROPENMID, wxSTC_MARK_MINUS, foldFgColour, foldBgColour);
        DefineMarker(wxSTC_MARKNUM_FOLDERMIDTAIL, wxSTC_MARK_BACKGROUND, foldFgColour, foldBgColour);

    } else { // use wxT("Arrows") as the default
        DefineMarker(wxSTC_MARKNUM_FOLDEROPEN, wxSTC_MARK_ARROWDOWN, foldFgColour, foldBgColour);
        DefineMarker(wxSTC_MARKNUM_FOLDER, wxSTC_MARK_ARROW, foldFgColour, foldBgColour);
        DefineMarker(wxSTC_MARKNUM_FOLDERSUB, wxSTC_MARK_BACKGROUND, foldFgColour, foldBgColour);
        DefineMarker(wxSTC_MARKNUM_FOLDERTAIL, wxSTC_MARK_BACKGROUND, foldFgColour, foldBgColour);
        DefineMarker(wxSTC_MARKNUM_FOLDEREND, wxSTC_MARK_ARROW, foldFgColour, foldBgColour);
        DefineMarker(wxSTC_MARKNUM_FOLDEROPENMID, wxSTC_MARK_ARROWDOWN, foldFgColour, foldBgColour);
        DefineMarker(wxSTC_MARKNUM_FOLDERMIDTAIL, wxSTC_MARK_BACKGROUND, foldFgColour, foldBgColour);
    }

    // Set the highlight colour for the folding
    for (int i = wxSTC_MARKNUM_FOLDEREND; i <= wxSTC_MARKNUM_FOLDEROPEN; ++i) {
        MarkerSetBackgroundSelected(i, lexer->IsDark() ? wxColour("YELLOW") : wxColour("RED"));
    }

    // Bookmark
    int marker = wxSTC_MARK_BOOKMARK;
    auto iter = ms_bookmarkShapes.find(options->GetBookmarkShape());
    if (iter != ms_bookmarkShapes.end()) {
        marker = iter->second;
    }

    for (size_t bmt = smt_FIRST_BMK_TYPE; bmt <= smt_LAST_BMK_TYPE; ++bmt) {
        MarkerDefine(bmt, marker);
        MarkerSetBackground(bmt, options->GetBookmarkBgColour(bmt - smt_FIRST_BMK_TYPE));
        MarkerSetForeground(bmt, options->GetBookmarkFgColour(bmt - smt_FIRST_BMK_TYPE));
    }

    // all bookmarks
    for (size_t bmt = smt_FIRST_BMK_TYPE; bmt <= smt_line_marker; ++bmt) {
        MarkerSetAlpha(bmt, 30);
    }

    // Breakpoints
    for (size_t bmt = smt_FIRST_BP_TYPE; bmt <= smt_LAST_BP_TYPE; ++bmt) {
        MarkerSetBackground(smt_breakpoint, "RED");
        MarkerSetAlpha(smt_breakpoint, 30);
    }

    wxBitmap breakpointBmp = clGetManager()->GetStdIcons()->LoadBitmap("breakpoint");
    wxBitmap breakpointCondBmp = clGetManager()->GetStdIcons()->LoadBitmap("breakpoint_cond");
    wxBitmap breakpointCmdList = clGetManager()->GetStdIcons()->LoadBitmap("breakpoint_cmdlist");
    wxBitmap breakpointIgnored = clGetManager()->GetStdIcons()->LoadBitmap("breakpoint_ignored");

    wxColour breakpointColour = wxColour("#FF5733");
    wxColour disabledColour = breakpointColour.ChangeLightness(165);
    wxColour defaultBgColour = StyleGetBackground(0); // Default style background colour

    MarkerDefine(smt_breakpoint, wxSTC_MARK_CIRCLE);
    this->MarkerSetBackground(smt_breakpoint, breakpointColour);
    this->MarkerSetForeground(smt_breakpoint, breakpointColour);

    MarkerDefine(smt_bp_disabled, wxSTC_MARK_CIRCLE);
    this->MarkerSetBackground(smt_bp_disabled, disabledColour);
    this->MarkerSetForeground(smt_bp_disabled, disabledColour);

    MarkerDefine(smt_bp_cmdlist, wxSTC_MARK_CHARACTER + 33); // !
    this->MarkerSetBackground(smt_bp_cmdlist, breakpointColour);
    this->MarkerSetForeground(smt_bp_cmdlist, breakpointColour);

    MarkerDefine(smt_bp_cmdlist_disabled, wxSTC_MARK_CHARACTER + 33); // !
    this->MarkerSetForeground(smt_bp_cmdlist, disabledColour);
    this->MarkerSetBackground(smt_bp_cmdlist, defaultBgColour);

    MarkerDefine(smt_bp_ignored, wxSTC_MARK_CHARACTER + 105); // i
    this->MarkerSetForeground(smt_bp_ignored, breakpointColour);
    this->MarkerSetBackground(smt_bp_ignored, defaultBgColour);

    MarkerDefine(smt_cond_bp, wxSTC_MARK_CHARACTER + 63); // ?
    this->MarkerSetForeground(smt_cond_bp, breakpointColour);
    this->MarkerSetBackground(smt_cond_bp, defaultBgColour);

    MarkerDefine(smt_cond_bp_disabled, wxSTC_MARK_CHARACTER + 63); // ?
    this->MarkerSetForeground(smt_cond_bp_disabled, disabledColour);
    this->MarkerSetBackground(smt_cond_bp_disabled, defaultBgColour);

    if (options->HasOption(OptionsConfig::Opt_Mark_Debugger_Line)) {
        MarkerDefine(smt_indicator, wxSTC_MARK_BACKGROUND, wxNullColour, options->GetDebuggerMarkerLine());
        MarkerSetAlpha(smt_indicator, 50);

    } else {
        MarkerDefine(smt_indicator, wxSTC_MARK_SHORTARROW);
        wxColour debuggerMarkerColour(136, 170, 0);
        MarkerSetBackground(smt_indicator, debuggerMarkerColour);
        MarkerSetForeground(smt_indicator, debuggerMarkerColour.ChangeLightness(50));
    }

    // warning and error markers
    MarkerDefine(smt_warning, wxSTC_MARK_SHORTARROW);
    MarkerSetForeground(smt_error, wxColor(128, 128, 0));
    MarkerSetBackground(smt_warning, wxColor(255, 215, 0));
    MarkerDefine(smt_error, wxSTC_MARK_SHORTARROW);
    MarkerSetForeground(smt_error, wxColor(128, 0, 0));
    MarkerSetBackground(smt_error, wxColor(255, 0, 0));

    CallTipSetBackground(wxSystemSettings::GetColour(wxSYS_COLOUR_INFOBK));
    CallTipSetForeground(wxSystemSettings::GetColour(wxSYS_COLOUR_INFOTEXT));

    SetTwoPhaseDraw(true);

#ifdef __WXMSW__
    SetBufferedDraw(true);
#endif

#if defined(__WXMAC__)
    // turning off these two greatly improves performance
    // on Mac
    SetLayoutCache(wxSTC_CACHE_DOCUMENT);
#else
    SetLayoutCache(wxSTC_CACHE_PAGE);
#endif

    // indentation settings
    SetTabIndents(true);
    SetBackSpaceUnIndents(true);

    // Should we use spaces or tabs for indenting?
    // Usually we will ask the configuration, however
    // when using Makefile we _must_ use the TABS
    SetUseTabs(FileExtManager::GetType(GetFileName().GetFullName()) == FileExtManager::TypeMakefile
                   ? true
                   : options->GetIndentUsesTabs());

    size_t tabWidth = options->GetTabWidth();
    SetTabWidth(tabWidth);

    size_t indentWidth = options->GetIndentWidth();
    SetIndent(indentWidth);

    int workspace_indent_width = GetWorkspaceIndentWidth();
    if (workspace_indent_width != wxNOT_FOUND) {
        SetTabWidth(workspace_indent_width);
        SetIndent(workspace_indent_width);
    }

    SetIndentationGuides(options->GetShowIndentationGuidelines() ? 3 : 0);

    size_t frame_flags = clMainFrame::Get()->GetFrameGeneralInfo().GetFlags();
    SetViewEOL(frame_flags & CL_SHOW_EOL ? true : false);

    IndicatorSetUnder(1, true);
    IndicatorSetUnder(INDICATOR_HYPERLINK, true);
    IndicatorSetUnder(INDICATOR_MATCH, false);
    IndicatorSetUnder(INDICATOR_DEBUGGER, true);

    bool isDarkTheme = (lexer && lexer->IsDark());
    auto indicator_style = isDarkTheme ? wxSTC_INDIC_BOX : wxSTC_INDIC_ROUNDBOX;
    SetUserIndicatorStyleAndColour(isDarkTheme ? wxSTC_INDIC_COMPOSITIONTHICK : wxSTC_INDIC_ROUNDBOX,
                                   isDarkTheme ? "PINK" : "RED");

    wxColour highlight_colour{ *wxGREEN };
    wxString val2 = EditorConfigST::Get()->GetString(wxT("WordHighlightColour"));
    if (!val2.empty()) {
        highlight_colour = wxColour(val2);
    }

    wxColour hover_highlight_colour = highlight_colour.ChangeLightness(150);

    int ALPHA = 100;
    if (isDarkTheme) {
        ALPHA = wxSTC_ALPHA_OPAQUE;
    }

    IndicatorSetForeground(1, options->GetBookmarkBgColour(smt_find_bookmark - smt_FIRST_BMK_TYPE));
    IndicatorSetHoverForeground(INDICATOR_WORD_HIGHLIGHT, true);
    IndicatorSetForeground(INDICATOR_WORD_HIGHLIGHT, highlight_colour);
    IndicatorSetStyle(INDICATOR_WORD_HIGHLIGHT, indicator_style);
    IndicatorSetAlpha(INDICATOR_WORD_HIGHLIGHT, ALPHA);

    IndicatorSetUnder(INDICATOR_FIND_BAR_WORD_HIGHLIGHT, !isDarkTheme);
    IndicatorSetStyle(INDICATOR_FIND_BAR_WORD_HIGHLIGHT, indicator_style);

    IndicatorSetForeground(INDICATOR_FIND_BAR_WORD_HIGHLIGHT, isDarkTheme ? "WHITE" : "BLACK");
    IndicatorSetAlpha(INDICATOR_FIND_BAR_WORD_HIGHLIGHT, ALPHA);

    IndicatorSetUnder(INDICATOR_CONTEXT_WORD_HIGHLIGHT, !isDarkTheme);
    IndicatorSetStyle(INDICATOR_CONTEXT_WORD_HIGHLIGHT, indicator_style);
    IndicatorSetForeground(INDICATOR_CONTEXT_WORD_HIGHLIGHT, isDarkTheme ? "WHITE" : "BLACK");
    IndicatorSetAlpha(INDICATOR_CONTEXT_WORD_HIGHLIGHT, ALPHA);

    IndicatorSetStyle(INDICATOR_HYPERLINK, wxSTC_INDIC_PLAIN);
    IndicatorSetStyle(INDICATOR_MATCH, indicator_style);
    IndicatorSetForeground(INDICATOR_MATCH, wxT("GREY"));

    IndicatorSetStyle(INDICATOR_DEBUGGER, indicator_style);
    IndicatorSetForeground(INDICATOR_DEBUGGER, wxT("GREY"));

    CmdKeyClear(wxT('L'), wxSTC_KEYMOD_CTRL); // clear Ctrl+D because we use it for something else

    // Set CamelCase caret movement
    if (options->GetCaretUseCamelCase()) {
        // selection
        CmdKeyAssign(wxSTC_KEY_LEFT, wxSTC_KEYMOD_CTRL | wxSTC_KEYMOD_SHIFT, wxSTC_CMD_WORDPARTLEFTEXTEND);
        CmdKeyAssign(wxSTC_KEY_RIGHT, wxSTC_KEYMOD_CTRL | wxSTC_KEYMOD_SHIFT, wxSTC_CMD_WORDPARTRIGHTEXTEND);

        // movement
        CmdKeyAssign(wxSTC_KEY_LEFT, wxSTC_KEYMOD_CTRL, wxSTC_CMD_WORDPARTLEFT);
        CmdKeyAssign(wxSTC_KEY_RIGHT, wxSTC_KEYMOD_CTRL, wxSTC_CMD_WORDPARTRIGHT);
    } else {
        // selection
        CmdKeyAssign(wxSTC_KEY_LEFT, wxSTC_KEYMOD_CTRL | wxSTC_KEYMOD_SHIFT, wxSTC_CMD_WORDLEFTEXTEND);
        CmdKeyAssign(wxSTC_KEY_RIGHT, wxSTC_KEYMOD_CTRL | wxSTC_KEYMOD_SHIFT, wxSTC_CMD_WORDRIGHTEXTEND);

        // movement
        CmdKeyAssign(wxSTC_KEY_LEFT, wxSTC_KEYMOD_CTRL, wxSTC_CMD_WORDLEFT);
        CmdKeyAssign(wxSTC_KEY_RIGHT, wxSTC_KEYMOD_CTRL, wxSTC_CMD_WORDRIGHT);
    }

#ifdef __WXOSX__
    CmdKeyAssign(wxSTC_KEY_DOWN, wxSTC_KEYMOD_CTRL, wxSTC_CMD_DOCUMENTEND);
    CmdKeyAssign(wxSTC_KEY_UP, wxSTC_KEYMOD_CTRL, wxSTC_CMD_DOCUMENTSTART);

    // OSX: wxSTC_KEYMOD_CTRL => CMD key
    CmdKeyAssign(wxSTC_KEY_RIGHT, wxSTC_KEYMOD_CTRL, wxSTC_CMD_LINEEND);
    CmdKeyAssign(wxSTC_KEY_LEFT, wxSTC_KEYMOD_CTRL, wxSTC_CMD_HOME);

    // OSX: wxSTC_KEYMOD_META => CONTROL key
    CmdKeyAssign(wxSTC_KEY_LEFT, wxSTC_KEYMOD_META, wxSTC_CMD_WORDPARTLEFT);
    CmdKeyAssign(wxSTC_KEY_RIGHT, wxSTC_KEYMOD_META, wxSTC_CMD_WORDPARTRIGHT);
#endif
    SetCurrentLineMarginStyle(GetCtrl());
}

void clEditor::OnSavePoint(wxStyledTextEvent& event)
{
    if (!GetIsVisible())
        return;

    wxString title;
    for (auto& [_line_number, status] : m_modifiedLines) {
        // mark all modified lines as "saved"
        if (status == LINE_MODIFIED) {
            status = LINE_SAVED;
        }
    }

    if (!GetModify() && m_trackChanges) {
        if (m_clearModifiedLines) {
            m_modifiedLines.clear();
            m_clearModifiedLines = false;
        }
        DoUpdateLineNumbers(GetOptions()->GetRelativeLineNumbers(), false);
    }

    clMainFrame::Get()->GetMainBook()->SetPageTitle(this, GetFileName(), GetModify());
    DoUpdateTLWTitle(false);
}

void clEditor::OnCharAdded(wxStyledTextEvent& event)
{
    bool hasSingleCaret = (GetSelections() == 1);
    OptionsConfigPtr options = GetOptions();
    if (m_prevSelectionInfo.IsOk()) {
        if (event.GetKey() == '"' && options->IsWrapSelectionWithQuotes()) {
            DoWrapPrevSelectionWithChars('"', '"');
            return;
        } else if (event.GetKey() == '[' && options->IsWrapSelectionBrackets()) {
            DoWrapPrevSelectionWithChars('[', ']');
            return;
        } else if (event.GetKey() == '\'' && options->IsWrapSelectionWithQuotes()) {
            DoWrapPrevSelectionWithChars('\'', '\'');
            return;
        } else if (event.GetKey() == '(' && options->IsWrapSelectionBrackets()) {
            DoWrapPrevSelectionWithChars('(', ')');
            return;
        } else if (event.GetKey() == '{' && options->IsWrapSelectionBrackets()) {
            DoWrapPrevSelectionWithChars('{', '}');
            return;
        }
    }

    // reset the flag

    m_prevSelectionInfo.Clear();
    bool addClosingBrace = m_autoAddNormalBraces && hasSingleCaret;
    bool addClosingDoubleQuotes = options->GetAutoCompleteDoubleQuotes() && hasSingleCaret;
    int pos = GetCurrentPos();
    bool canShowCompletionBox(true);
    // make sure line is visible
    int curLine = LineFromPosition(pos);
    if (!GetFoldExpanded(curLine)) {
        DoToggleFold(curLine, "...");
    }

    bool bJustAddedIndicator = false;
    int nextChar = SafeGetChar(pos), prevChar = SafeGetChar(pos - 2);

    //-------------------------------------
    // Smart quotes management
    //-------------------------------------
    if (addClosingDoubleQuotes) {
        if ((event.GetKey() == '"' || event.GetKey() == '\'') && event.GetKey() == GetCharAt(pos)) {
            CharRight();
            DeleteBack();
        } else if (!wxIsalnum(nextChar) && !wxIsalnum(prevChar)) {
            // add complete quotes; but don't if the next char is alnum,
            // which is annoying if you're trying to retrofit quotes around a string!
            // Also not if the previous char is alnum: it's more likely (especially in non-code editors)
            // that someone is trying to type _don't_ than it's a burning desire to write _don''_
            if (event.GetKey() == wxT('"') && !m_context->IsCommentOrString(pos)) {
                InsertText(pos, wxT("\""));
                SetIndicatorCurrent(INDICATOR_MATCH);
                IndicatorFillRange(pos, 1);
                bJustAddedIndicator = true;

            } else if (event.GetKey() == wxT('\'') && !m_context->IsCommentOrString(pos)) {
                InsertText(pos, wxT("'"));
                SetIndicatorCurrent(INDICATOR_MATCH);
                IndicatorFillRange(pos, 1);
                bJustAddedIndicator = true;
            }
        }
    }
    //-------------------------------------
    // Smart quotes management
    //-------------------------------------
    if (!bJustAddedIndicator && IndicatorValueAt(INDICATOR_MATCH, pos) && event.GetKey() == GetCharAt(pos)) {
        CharRight();
        DeleteBack();

    } else if (m_smartParen && (event.GetKey() == ')' || event.GetKey() == ']') && event.GetKey() == GetCharAt(pos)) {
        // disable the auto brace adding when inside comment or string
        if (!m_context->IsCommentOrString(pos)) {
            CharRight();
            DeleteBack();
        }
    }

    wxChar matchChar(0);
    switch (event.GetKey()) {
    case ';':
        if (!m_disableSemicolonShift && !m_context->IsCommentOrString(pos))
            m_context->SemicolonShift();
        break;
    case '@':  // PHP / Java document style
    case '\\': // Qt Style
        if (m_context->IsAtBlockComment()) {
            m_context->BlockCommentComplete();
        }
        break;
    case '(':
        if (m_context->IsCommentOrString(GetCurrentPos()) == false) {
            // trigger a code complete for function calltip.
            wxCommandEvent event{ wxEVT_MENU, XRCID("function_call_tip") };
            EventNotifier::Get()->TopFrame()->GetEventHandler()->AddPendingEvent(event);
        }
        matchChar = ')';
        break;
    case '[':
        matchChar = ']';
        break;

    case '{':
        m_context->AutoIndent(event.GetKey());
        matchChar = '}';
        break;

    case ':':
        m_context->AutoIndent(event.GetKey());
        break;

    case ')':
        // Remove one tip from the queue. If the queue new size is 0
        // the tooltip is then cancelled
        GetFunctionTip()->Remove();
        break;

    case '}':
        m_context->AutoIndent(event.GetKey());
        break;
    case '\n': {
        long matchedPos(wxNOT_FOUND);
        // incase ENTER was hit immediately after we inserted '{' into the code...
        if (m_lastCharEntered == wxT('{') &&                         // Last char entered was {
            m_autoAddMatchedCurlyBrace &&                            // auto-add-match-brace option is enabled
            !m_disableSmartIndent &&                                 // the disable smart indent option is NOT enabled
            MatchBraceBack(wxT('}'), GetCurrentPos(), matchedPos) && // Insert it only if it match an open brace
            !m_context->IsDefaultContext() &&                        // the editor's context is NOT the default one
            matchedPos == m_lastCharEnteredPos) { // and that open brace must be the one that we have inserted

            matchChar = '}';

            BeginUndoAction();
            // Check to see if there are more chars on the line
            int curline = GetCurrentLine();

            // get the line end position, but without the EOL
            int lineEndPos = LineEnd(curline) - GetEolString().length();
            wxString restOfLine = GetTextRange(pos, lineEndPos);
            wxString restOfLineTrimmed = restOfLine;
            restOfLineTrimmed.Trim().Trim(false);
            bool shiftCode = (!restOfLineTrimmed.StartsWith(")")) && (!restOfLineTrimmed.IsEmpty());

            if (shiftCode) {
                SetSelection(pos, lineEndPos);
                ReplaceSelection("");
            }
            InsertText(pos, matchChar);
            CharRight();
            m_context->AutoIndent(wxT('}'));
            InsertText(pos, GetEolString());
            CharRight();
            SetCaretAt(pos);
            if (shiftCode) {
                // restore the content that we just removed
                InsertText(pos, restOfLine);
            }

            m_context->AutoIndent(wxT('\n'));
            EndUndoAction();

        } else {

            m_context->AutoIndent(event.GetKey());

            // incase we are typing in a folded line, make sure it is visible
            EnsureVisible(curLine + 1);
        }
    }

    break;
    default:
        break;
    }

    // Check for code completion strings
    wxChar charTyped = event.GetKey();
    // get the previous char. Note that the current position is already *after* the
    // current char, so we need to go back 2 chars
    wxChar firstChar = SafeGetChar(GetCurrentPos() - 2);

    wxString strTyped, strTyped2;
    strTyped << charTyped;
    strTyped2 << firstChar << charTyped;

    CompletionHelper helper;
    if (helper.is_include_statement(GetLine(GetCurrentLine()), nullptr, nullptr)) {
        CallAfter(&clEditor::CompleteWord, LSP::CompletionItem::kTriggerUser, false);
    } else if ((GetContext()->IsStringTriggerCodeComplete(strTyped) ||
                GetContext()->IsStringTriggerCodeComplete(strTyped2)) &&
               !GetContext()->IsCommentOrString(GetCurrentPos())) {
        // this char should trigger a code completion
        CallAfter(&clEditor::CodeComplete);
    }

    if (matchChar && !m_disableSmartIndent && !m_context->IsCommentOrString(pos)) {
        if (matchChar == ')' && addClosingBrace) {
            // Only add a close brace if the next char is whitespace
            // or if it's an already-matched ')' (which keeps things syntactically correct)
            long matchedPos(wxNOT_FOUND);
            int nextChar = SafeGetChar(pos);
            switch (nextChar) {
            case ')':
                if (!MatchBraceBack(matchChar, PositionBeforePos(pos), matchedPos)) {
                    break;
                }
            case ' ':
            case '\t':
            case '\n':
            case '\r':
                InsertText(pos, matchChar);
                SetIndicatorCurrent(INDICATOR_MATCH);
                // use grey colour rather than black, otherwise this indicator is invisible when using the
                // black theme
                IndicatorFillRange(pos, 1);
                break;
            }
        } else if (matchChar != '}' && addClosingBrace) {
            InsertText(pos, matchChar);
            SetIndicatorCurrent(INDICATOR_MATCH);
            // use grey colour rather than black, otherwise this indicator is invisible when using the
            // black theme
            IndicatorFillRange(pos, 1);
        }
    }

    // Show the completion box if needed. canShowCompletionBox is set to false only if it was just dismissed
    // at the top of this function
    if (!IsCompletionBoxShown() && canShowCompletionBox) {
        // display the keywords completion box only if user typed more than 2
        // chars && the caret is placed at the end of that word
        long startPos = WordStartPosition(pos, true);
        bool min_chars_typed = (pos - startPos) >= (TagsManagerST::Get()->GetCtagsOptions().GetMinWordLen());
        if ((GetWordAtCaret().Len() >= 2) && min_chars_typed) {
            // trigger the CC on the Paint event
            m_trigger_cc_at_pos = GetCurrentPosition();
        }
    }

    if (event.GetKey() != 13) {
        // Dont store last character if it was \r
        m_lastCharEntered = event.GetKey();

        // Since we already entered the character...
        m_lastCharEnteredPos = PositionBefore(GetCurrentPos());
    }

    event.Skip();
}

void clEditor::SetEnsureCaretIsVisible(int pos, bool preserveSelection /*=true*/)
{
    DoEnsureCaretIsVisible(pos, preserveSelection);
}

void clEditor::OnScnPainted(wxStyledTextEvent& event) { event.Skip(); }

void clEditor::DoEnsureCaretIsVisible(int pos, bool preserveSelection)
{
    int start = -1, end = -1;
    if (preserveSelection) {
        start = GetSelectionStart();
        end = GetSelectionEnd();
    }

    SetCaretAt(pos);

    // and finally restore any selection if requested
    if (preserveSelection && (start != end)) {
        this->SetSelection(start, end);
    }
}

void clEditor::OnSciUpdateUI(wxStyledTextEvent& event)
{
    event.Skip();

    m_scrollbar_recalc_is_required = true;

    // keep the last line we visited this method
    int lastLine = m_editorState.current_line;

    // Update the line numbers if needed (only when using custom drawing line numbers)
    UpdateLineNumbers(false);

    // Get current position
    long curpos = GetCurrentPos();

    // ignore << and >>
    int charAfter = SafeGetChar(PositionAfter(curpos));
    int charBefore = SafeGetChar(PositionBefore(curpos));
    int beforeBefore = SafeGetChar(PositionBefore(PositionBefore(curpos)));
    int charCurrnt = SafeGetChar(curpos);

    const int selectionStart = GetSelectionStart();
    const int selectionEnd = GetSelectionEnd();
    const int selectionSize = std::abs(selectionEnd - selectionStart);
    const int selectionLn = std::abs(LineFromPosition(selectionEnd) - LineFromPosition(selectionStart)) + 1;
    int mainSelectionPos = GetSelectionNCaret(GetMainSelection());
    int curLine = LineFromPosition(mainSelectionPos);

    if (m_trigger_cc_at_pos > 0) {
        // trigger CC
        m_context->CallAfter(&ContextBase::OnUserTypedXChars, m_trigger_cc_at_pos - 1);
        m_trigger_cc_at_pos = wxNOT_FOUND;
    }

    SetIndicatorCurrent(INDICATOR_MATCH);
    IndicatorClearRange(0, curpos);

    int end = PositionFromLine(curLine + 1);
    if (end >= curpos && end < GetTextLength()) {
        IndicatorClearRange(end, GetTextLength() - end);
    }

    // get the current position
    if (curLine != lastLine) {
        clCodeCompletionEvent evtUpdateNavBar(wxEVT_CC_UPDATE_NAVBAR);
        evtUpdateNavBar.SetLineNumber(curLine);
        evtUpdateNavBar.SetFileName(FileUtils::RealPath(GetFileName().GetFullPath()));
        EventNotifier::Get()->AddPendingEvent(evtUpdateNavBar);
    }

    if (curpos != m_lastUpdatePosition) {
        // update the status bar
        m_lastUpdatePosition = curpos;
        wxString message;
        int curLine = LineFromPosition(curpos);

        if (m_statusBarFields & kShowLine) {
            message << "Ln " << curLine + 1;
        }
        if (m_statusBarFields & kShowColumn) {
            message << (!message.empty() ? ", " : "") << "Col " << GetColumn(curpos);
        }
        if (m_statusBarFields & kShowLineCount) {
            message << (!message.empty() ? ", " : "") << "Lns " << GetLineCount();
        }
        if (m_statusBarFields & kShowPosition) {
            message << (!message.empty() ? ", " : "") << "Pos " << curpos;
        }
        if (m_statusBarFields & kShowLen) {
            message << (!message.empty() ? ", " : "") << "Len " << GetLength();
        }
        if ((m_statusBarFields & kShowSelectedChars) && selectionSize) {
            message << (!message.empty() ? ", " : "") << "Sel " << selectionSize;
        }
        if ((m_statusBarFields & kShowSelectedLines) && selectionSize && selectionLn) {
            message << (!message.empty() ? ", " : "") << "SelLn " << selectionLn;
        }

        // Always update the status bar with event, calling it directly causes performance degradation
        m_mgr->GetStatusBar()->SetLinePosColumn(message);
#ifdef __WXGTK__
        // the status bar does not refresh on Linux automatically
        m_mgr->GetStatusBar()->Refresh();
#endif
    }

    DoBraceMatching();

    // let the context handle this as well
    m_context->OnSciUpdateUI(event);

    // Keep the current state
    m_editorState = EditorViewState::From(this);
}

void clEditor::OnMarginClick(wxStyledTextEvent& event)
{
    int nLine = LineFromPosition(event.GetPosition());
    switch (event.GetMargin()) {
    case SYMBOLS_MARGIN_ID:
        // symbols / breakpoints margin
        {
            // If we have a compiler error here -> it takes precedence
            if ((MarkerGet(nLine) & mmt_compiler) && m_compilerMessagesMap.count(nLine)) {
                // user clicked on compiler error, fire an event
                clEditorEvent event_error_clicked{ wxEVT_EDITOR_MARGIN_CLICKED };
                event_error_clicked.SetUserData(m_compilerMessagesMap.find(nLine)->second.userData.get());
                event_error_clicked.SetFileName(GetRemotePathOrLocal());
                event_error_clicked.SetLineNumber(nLine);
                // use process here and not AddPendingEvent or QueueEvent
                if (EventNotifier::Get()->ProcessEvent(event_error_clicked)) {
                    return;
                }
            }

            if (event.GetShift()) {
                // Shift-LeftDown, let the user drag any breakpoint marker
                int markers = (MarkerGet(nLine) & mmt_all_breakpoints);
                if (!markers) {
                    break;
                }
                // There doesn't seem to be an elegant way to get the defined bitmap for a marker
                wxBitmap bm;
                if (markers & mmt_bp_disabled) {
                    bm = wxBitmap(wxImage(BreakptDisabled));

                } else if (markers & mmt_bp_cmdlist) {
                    bm = wxBitmap(wxImage(BreakptCommandList));

                } else if (markers & mmt_bp_cmdlist_disabled) {
                    bm = wxBitmap(wxImage(BreakptCommandListDisabled));

                } else if (markers & mmt_bp_ignored) {
                    bm = wxBitmap(wxImage(BreakptIgnore));

                } else if (markers & mmt_cond_bp) {
                    bm = wxBitmap(wxImage(ConditionalBreakpt));

                } else if (markers & mmt_cond_bp_disabled) {
                    bm = wxBitmap(wxImage(ConditionalBreakptDisabled));

                } else {
                    // Make the standard bp bitmap the default
                    bm = wxBitmap(wxImage(stop_xpm));
                }

                // The breakpoint manager organises the actual drag/drop
                BreakptMgr* bpm = ManagerST::Get()->GetBreakpointsMgr();
                bpm->DragBreakpoint(this, nLine, bm);

                Connect(wxEVT_MOTION, wxMouseEventHandler(myDragImage::OnMotion), NULL, bpm->GetDragImage());
                Connect(wxEVT_LEFT_UP, wxMouseEventHandler(myDragImage::OnEndDrag), NULL, bpm->GetDragImage());

            } else {
                GotoPos(event.GetPosition());
                ToggleBreakpoint();
            }
        }
        break;
    case FOLD_MARGIN_ID:
        // fold margin
        {
            DoToggleFold(nLine, "...");

            int caret_pos = GetCurrentPos();
            if (caret_pos != wxNOT_FOUND) {
                int caret_line = LineFromPosition(caret_pos);
                if (caret_line != wxNOT_FOUND && GetLineVisible(caret_line) == false) {
                    // the caret line is hidden (i.e. stuck in a fold) so set it somewhere else
                    while (caret_line >= 0) {
                        if ((GetFoldLevel(caret_line) & wxSTC_FOLDLEVELHEADERFLAG) && GetLineVisible(caret_line)) {
                            SetCaretAt(PositionFromLine(caret_line));
                            break;
                        }
                        caret_line--;
                    }
                }
            }

            // Try to make as much as possible of the originally-displayed code stay in the same screen position
            // That's no problem if the fold-head is visible: that line and above automatically stay in place
            // However if it's off screen and the user clicks in a margin to fold, no part of the function will stay on
            // screen
            // The following code scrolls the correct amount to keep the position of the lines *below* the function
            // unchanged
            // This also brings the newly-folded function into view.
            // NB It fails if the cursor was originally inside the new fold; but at least then the fold head gets shown
            int foldparent = GetFoldParent(nLine);
            int firstvisibleline = GetFirstVisibleLine();
            if (!(GetFoldLevel(nLine) & wxSTC_FOLDLEVELHEADERFLAG) // If the click was below the fold head
                && (foldparent < firstvisibleline)) {              // and the fold head is off the screen
                int linestoscroll = foldparent - GetLastChild(foldparent, -1);
                // If there are enough lines above the screen to scroll downwards, do so
                if ((firstvisibleline + linestoscroll) >= 0) { // linestoscroll will always be negative
                    LineScroll(0, linestoscroll);
                }
            }
        }
        break;
    default:
        break;
    }
}

void clEditor::DefineMarker(int marker, int markerType, wxColor fore, wxColor back)
{
    MarkerDefine(marker, markerType);
    MarkerSetForeground(marker, fore);
    MarkerSetBackground(marker, back);
}

bool clEditor::SaveFile()
{
    if (!this->GetModify()) {
        return true;
    }

    if (!GetFileName().FileExists()) {
        return SaveFileAs();
    }

    // first save the file content
    if (!SaveToFile(m_fileName))
        return false;

    // if we managed to save the file, remove the 'read only' attribute
    clMainFrame::Get()->GetMainBook()->MarkEditorReadOnly(this);

    // Take a snapshot of the current deltas. We'll need this as a 'base' for any future FindInFiles call
    m_deltas->OnFileSaved();
    return true;
}

bool clEditor::SaveFileAs(const wxString& newname, const wxString& savePath)
{
    // Prompt the user for a new file name
    const wxString ALL(wxT("All Files (*)|*"));
    wxFileDialog dlg(this,
                     _("Save As"),
                     savePath.IsEmpty() ? m_fileName.GetPath() : savePath,
                     newname.IsEmpty() ? m_fileName.GetFullName() : newname,
                     ALL,
                     wxFD_SAVE | wxFD_OVERWRITE_PROMPT,
                     wxDefaultPosition);

    if (dlg.ShowModal() != wxID_OK) {
        return false;
    }

    // get the path
    wxFileName name(dlg.GetPath());

    // Prepare the "SaveAs" event, but dont send it just yet
    clFileSystemEvent saveAsEvent(wxEVT_FILE_SAVEAS);
    saveAsEvent.SetPath(m_fileName.Exists() ? m_fileName.GetFullPath() : wxString(""));
    saveAsEvent.SetNewpath(name.GetFullPath());

    if (!SaveToFile(name)) {
        wxMessageBox(_("Failed to save file"), _("Error"), wxOK | wxICON_ERROR);
        return false;
    }
    m_fileName = name;

    // update the tab title (again) since we really want to trigger an update to the file tooltip
    clMainFrame::Get()->GetMainBook()->SetPageTitle(this, m_fileName, false);
    DoUpdateTLWTitle(false);

    // update syntax highlight
    SetSyntaxHighlight();

    clMainFrame::Get()->GetMainBook()->MarkEditorReadOnly(this);

    // Fire the "File renamed" event
    EventNotifier::Get()->AddPendingEvent(saveAsEvent);
    return true;
}

// an internal function that does the actual file writing to disk
bool clEditor::SaveToFile(const wxFileName& fileName)
{
    {
        // Notify about file being saved
        clCommandEvent beforeSaveEvent(wxEVT_BEFORE_EDITOR_SAVE);
        beforeSaveEvent.SetFileName(GetRemotePathOrLocal());
        EventNotifier::Get()->ProcessEvent(beforeSaveEvent);

        if (!beforeSaveEvent.IsAllowed()) {
            // A plugin vetoed the file save
            return false;
        }
    }

    // Do all the writing on the temporary file
    wxFileName intermediateFile(fileName);

    // Make sure that the folder does exist
    intermediateFile.Mkdir(wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);
    intermediateFile.SetFullName("~" + fileName.GetFullName() + "." + ::wxGetUserId());

    {
        // Ensure that a temporary file with this name does not exist
        FileUtils::Deleter deleter(intermediateFile);
    }

    // Ensure that the temporary file that we will be creating
    // is removed when leaving the function
    FileUtils::Deleter deleter(intermediateFile);

    // save the file using the user's defined encoding
    // unless we got a BOM set
    wxCSConv fontEncConv(GetOptions()->GetFileFontEncoding());
    bool useBuiltIn = (GetOptions()->GetFileFontEncoding() == wxFONTENCODING_UTF8);

    // trim lines / append LF if needed
    TrimText(GetOptions()->GetTrimLine(), GetOptions()->GetAppendLF());

    // BUG#2982452
    // try to manually convert the text to make sure that the conversion does not fail
    wxString theText = GetText();

    // If the intermediate file exists, it means that we got problems deleting it (usually permissions)
    // Notify the user and continue
    if (intermediateFile.Exists()) {
        // We failed to delete the intermediate file
        ::wxMessageBox(
            wxString::Format(_("Unable to create intermediate file\n'%s'\nfor writing. File already exists!"),
                             intermediateFile.GetFullPath()),
            "CodeLite",
            wxOK | wxCENTER | wxICON_ERROR,
            EventNotifier::Get()->TopFrame());
        return false;
    }

    wxFFile file(intermediateFile.GetFullPath().GetData(), "wb");
    if (!file.IsOpened()) {
        // Nothing to be done
        wxMessageBox(wxString::Format(_("Failed to open file\n'%s'\nfor write"), fileName.GetFullPath()),
                     "CodeLite",
                     wxOK | wxCENTER | wxICON_ERROR);
        return false;
    }

    // Convert the text
    const wxWX2MBbuf buf = theText.mb_str(useBuiltIn ? (const wxMBConv&)wxConvUTF8 : (const wxMBConv&)fontEncConv);
    if (!buf.data()) {
        wxMessageBox(wxString::Format(wxT("%s\n%s '%s'"),
                                      _("Save file failed!"),
                                      _("Could not convert the file to the requested encoding"),
                                      wxFontMapper::GetEncodingName(GetOptions()->GetFileFontEncoding())),
                     "CodeLite",
                     wxOK | wxICON_WARNING);
        return false;
    }

    if ((buf.length() == 0) && !theText.IsEmpty()) {
        // something went wrong in the conversion process
        wxString errmsg;
        errmsg << _("File text conversion failed!\nCheck your file font encoding from\nSettings | Preferences | "
                    "Misc | Locale");
        wxMessageBox(errmsg, "CodeLite", wxOK | wxICON_ERROR | wxCENTER, wxTheApp->GetTopWindow());
        return false;
    }

    if (!m_fileBom.IsEmpty()) {
        // restore the BOM
        file.Write(m_fileBom.GetData(), m_fileBom.Len());
    }
    file.Write(buf.data(), strlen(buf.data()));
    file.Close();

    wxFileName symlinkedFile = fileName;
    if (FileUtils::IsSymlink(fileName)) {
        symlinkedFile = FileUtils::wxReadLink(fileName);
    }

    // keep the original file permissions
    mode_t origPermissions = 0;
    if (!FileUtils::GetFilePermissions(symlinkedFile, origPermissions)) {
        clWARNING() << "Failed to read file permissions." << fileName << clEndl;
    }

    // If this file is not writable, prompt the user before we do something stupid
    if (symlinkedFile.FileExists() && !symlinkedFile.IsFileWritable()) {
        // Prompt the user
        if (::wxMessageBox(wxString() << _("The file\n") << fileName.GetFullPath()
                                      << _("\nis a read only file, continue?"),
                           "CodeLite",
                           wxYES_NO | wxCANCEL | wxCANCEL_DEFAULT | wxICON_WARNING,
                           EventNotifier::Get()->TopFrame()) != wxYES) {
            return false;
        }
    }

// The write was done to a temporary file, override it
#ifdef __WXMSW__
    if (!::wxRenameFile(intermediateFile.GetFullPath(), symlinkedFile.GetFullPath(), true)) {
        // Check if the file has the ReadOnly attribute and attempt to remove it
        if (MSWRemoveROFileAttribute(symlinkedFile)) {
            if (!::wxRenameFile(intermediateFile.GetFullPath(), symlinkedFile.GetFullPath(), true)) {
                wxMessageBox(
                    wxString::Format(_("Failed to override read-only file")), "CodeLite", wxOK | wxICON_WARNING);
                return false;
            }
        }
    }
#else
    if (!::wxRenameFile(intermediateFile.GetFullPath(), symlinkedFile.GetFullPath(), true)) {
        // Try clearing the clang cache and try again
        wxMessageBox(wxString::Format(_("Failed to override read-only file")), "CodeLite", wxOK | wxICON_WARNING);
        return false;
    }
#endif

    // Restore the orig file permissions
    if (origPermissions) {
        FileUtils::SetFilePermissions(symlinkedFile, origPermissions);
    }

    // update the modification time of the file
    m_modifyTime = FileUtils::GetFileModificationTime(symlinkedFile);
    SetSavePoint();

    // update the tab title (remove the star from the file name)
    clMainFrame::Get()->GetMainBook()->SetPageTitle(this, fileName, false);

    // Update line numbers drawings
    UpdateLineNumberMarginWidth();
    UpdateLineNumbers(true);

    if (fileName.GetExt() != m_fileName.GetExt()) {
        // new context is required
        SetSyntaxHighlight();
    }

    // Fire a wxEVT_FILE_SAVED event
    EventNotifier::Get()->PostFileSavedEvent(GetRemotePathOrLocal());
    return true;
}

// this function is called before the debugger startup
void clEditor::UpdateBreakpoints()
{
    wxString file_path = GetRemotePathOrLocal();
    // if this is a remote file, use that path in the debugger view
    ManagerST::Get()->GetBreakpointsMgr()->DeleteAllBreakpointsByFileName(file_path);

    // iterate over the array and update the breakpoint manager with updated line numbers for each breakpoint
    for (auto& d : m_breakpointsInfo) {
        int handle = d.first;
        int line = MarkerLineFromHandle(handle);
        if (line >= 0) {
            for (size_t i = 0; i < d.second.size(); i++) {
                d.second[i].lineno = line + 1;
                d.second[i].origin = BO_Editor;
                d.second[i].file = file_path;
            }
        }

        ManagerST::Get()->GetBreakpointsMgr()->SetBreakpoints(d.second);

        // update the Breakpoints pane too
        clMainFrame::Get()->GetDebuggerPane()->GetBreakpointView()->Initialize();
    }
}

wxString clEditor::GetWordAtCaret(bool wordCharsOnly) { return GetWordAtPosition(GetCurrentPos(), wordCharsOnly); }

//---------------------------------------------------------------------------
// Most of the functionality for this functionality
// is done in the Language & TagsManager objects, however,
// as you can see below, much work still needs to be done in the application
// layer (outside of the library) to provide the input arguments for
// the CodeParser library
//---------------------------------------------------------------------------
void clEditor::CompleteWord(LSP::CompletionItem::eTriggerKind triggerKind, bool onlyRefresh)
{
    if (AutoCompActive())
        return; // Don't clobber the boxes

    wxString fullpath = FileUtils::RealPath(GetFileName().GetFullPath());

    if (triggerKind == LSP::CompletionItem::kTriggerUser) {
        // user hit Ctrl-SPACE
        clCodeCompletionEvent evt(wxEVT_CC_CODE_COMPLETE);
        evt.SetPosition(GetCurrentPosition());
        evt.SetInsideCommentOrString(m_context->IsCommentOrString(PositionBefore(GetCurrentPos())));
        evt.SetTriggerKind(triggerKind);
        evt.SetFileName(fullpath);
        EventNotifier::Get()->AddPendingEvent(evt);
        return;
    } else {
        if (GetContext()->IsAtBlockComment()) {
            // Check if the current word starts with \ or @
            int wordStartPos = GetFirstNonWhitespacePos(true);
            if (wordStartPos != wxNOT_FOUND) {
                wxChar firstChar = GetCtrl()->GetCharAt(wordStartPos);
                if ((firstChar == '@') || (firstChar == '\\')) {
                    // Change the event to wxEVT_CC_BLOCK_COMMENT_WORD_COMPLETE
                    clCodeCompletionEvent evt(wxEVT_CC_BLOCK_COMMENT_WORD_COMPLETE);
                    evt.SetPosition(GetCurrentPosition());
                    evt.SetInsideCommentOrString(m_context->IsCommentOrString(PositionBefore(GetCurrentPos())));
                    evt.SetTriggerKind(triggerKind);
                    evt.SetFileName(fullpath);
                    // notice the difference that we fire it using EventNotifier!
                    EventNotifier::Get()->AddPendingEvent(evt);
                    return;
                }
            }
        }
    }

    // Let the plugins a chance to override the default behavior
    // 24x7 CC (as-we-type)
    if (!GetContext()->IsAtBlockComment() && !GetContext()->IsAtLineComment()) {
        clCodeCompletionEvent evt(wxEVT_CC_CODE_COMPLETE);
        evt.SetPosition(GetCurrentPosition());
        evt.SetInsideCommentOrString(m_context->IsCommentOrString(PositionBefore(GetCurrentPos())));
        evt.SetTriggerKind(triggerKind);
        evt.SetFileName(fullpath);
        EventNotifier::Get()->AddPendingEvent(evt);
    }
}

//------------------------------------------------------------------
// AutoCompletion, by far the nicest feature of a modern IDE
// This function attempts to resolve the string to the left of
// the '.', '->' operator and to display a popup menu with
// list of possible matches
//------------------------------------------------------------------
void clEditor::CodeComplete()
{
    if (AutoCompActive())
        return; // Don't clobber the boxes..

    clCodeCompletionEvent evt(wxEVT_CC_CODE_COMPLETE);
    evt.SetPosition(GetCurrentPosition());
    evt.SetTriggerKind(LSP::CompletionItem::kTriggerKindInvoked);
    evt.SetInsideCommentOrString(m_context->IsCommentOrString(PositionBefore(GetCurrentPos())));
    evt.SetFileName(FileUtils::RealPath(GetFileName().GetFullPath()));
    EventNotifier::Get()->AddPendingEvent(evt);
}

void clEditor::GotoDefinition()
{
    // Let the plugins process this first
    wxString word = GetWordAtCaret();
    clCodeCompletionEvent event(wxEVT_CC_FIND_SYMBOL, GetId());
    event.SetWord(word);
    event.SetPosition(GetCurrentPosition());
    event.SetInsideCommentOrString(m_context->IsCommentOrString(PositionBefore(GetCurrentPos())));
    event.SetFileName(FileUtils::RealPath(GetFileName().GetFullPath()));
    EventNotifier::Get()->ProcessEvent(event);
}

void clEditor::OnDwellStart(wxStyledTextEvent& event)
{
    // First see if we're hovering over a breakpoint or build marker
    // Assume anywhere to the left of the fold margin qualifies
    int margin = 0;
    wxPoint pt(ScreenToClient(wxGetMousePosition()));
    wxRect clientRect = GetClientRect();

    // If the mouse is no longer over the editor, cancel the tooltip
    if (!clientRect.Contains(pt)) {
        return;
    }

    // Always cancel the previous tooltip...
    DoCancelCodeCompletionBox();

    for (int n = 0; n < LAST_MARGIN_ID; ++n) {
        margin += GetMarginWidth(n);
    }

    if (IsContextMenuOn() || IsDragging() || !GetSTCFocus()) {
        // Don't cover the context menu or a potential drop-point with a calltip!
        // And, especially, try to avoid scintilla's party-piece: placing a permanent calltip on top of some
        // innocent app!

    } else if (event.GetX() > 0 // It seems that we can get spurious events with x == 0
               && event.GetX() < margin) {

        // We can't use event.GetPosition() here, as in the margin it returns -1
        int position = PositionFromPoint(wxPoint(event.GetX(), event.GetY()));
        int line = LineFromPosition(position);
        wxString tooltip, title;
        wxString fname = FileUtils::RealPath(GetFileName().GetFullPath());

        if (MarkerGet(line) & mmt_all_breakpoints) {
            ManagerST::Get()->GetBreakpointsMgr()->GetTooltip(fname, line + 1, tooltip, title);
        }

        else if (MarkerGet(line) & mmt_all_bookmarks) {
            GetBookmarkTooltip(line, tooltip, title);
        }

        // Compiler marker takes precedence over any other tooltip on that margin
        if ((MarkerGet(line) & mmt_compiler) && m_compilerMessagesMap.count(line)) {
            // Get the compiler tooltip
            tooltip = m_compilerMessagesMap.find(line)->second.message;
            // Disable markdown to ensure it doesn't break anything
            StringUtils::DisableMarkdownStyling(tooltip);
        }

        if (!tooltip.IsEmpty()) {
            DoShowCalltip(wxNOT_FOUND, title, tooltip, false);
        }

    } else if (ManagerST::Get()->DbgCanInteract() && clientRect.Contains(pt)) {
        m_context->OnDbgDwellStart(event);

    } else if (TagsManagerST::Get()->GetCtagsOptions().GetFlags() & CC_DISP_TYPE_INFO) {

        // Allow the plugins to override the default built-in behavior of displaying
        // the type info tooltip
        clCodeCompletionEvent evtTypeinfo(wxEVT_CC_TYPEINFO_TIP, GetId());
        evtTypeinfo.SetPosition(event.GetPosition());
        evtTypeinfo.SetInsideCommentOrString(m_context->IsCommentOrString(event.GetPosition()));
        evtTypeinfo.SetFileName(FileUtils::RealPath(GetFileName().GetFullPath()));
        if (EventNotifier::Get()->ProcessEvent(evtTypeinfo)) {
            if (!evtTypeinfo.GetTooltip().IsEmpty()) {
                DoShowCalltip(wxNOT_FOUND, "", evtTypeinfo.GetTooltip());
            }
        }
    }
}

void clEditor::OnDwellEnd(wxStyledTextEvent& event)
{
    DoCancelCalltip();
    m_context->OnDwellEnd(event);
    m_context->OnDbgDwellEnd(event);
}

void clEditor::OnCallTipClick(wxStyledTextEvent& event) { m_context->OnCallTipClick(event); }

void clEditor::OnMenuCommand(wxCommandEvent& event)
{
    MenuEventHandlerPtr handler = MenuManager::Get()->GetHandler(event.GetId());

    if (handler) {
        handler->ProcessCommandEvent(this, event);
    }
}

void clEditor::OnUpdateUI(wxUpdateUIEvent& event)
{
    MenuEventHandlerPtr handler = MenuManager::Get()->GetHandler(event.GetId());

    if (handler) {
        handler->ProcessUpdateUIEvent(this, event);
    }
}

//-----------------------------------------------------------------------
// Misc functions
//-----------------------------------------------------------------------

wxString clEditor::PreviousWord(int pos, int& foundPos)
{
    // Get the partial word that we have
    wxChar ch = 0;
    long curpos = PositionBefore(pos);
    if (curpos == 0) {
        foundPos = wxNOT_FOUND;
        return wxT("");
    }

    while (true) {
        ch = GetCharAt(curpos);
        if (ch == wxT('\t') || ch == wxT(' ') || ch == wxT('\r') || ch == wxT('\v') || ch == wxT('\n')) {
            long tmpPos = curpos;
            curpos = PositionBefore(curpos);
            if (curpos == 0 && tmpPos == curpos)
                break;
        } else {
            long start = WordStartPosition(curpos, true);
            long end = WordEndPosition(curpos, true);
            return GetTextRange(start, end);
        }
    }
    foundPos = wxNOT_FOUND;
    return wxT("");
}

wxChar clEditor::PreviousChar(const int& pos, int& foundPos, bool wantWhitespace)
{
    wxChar ch = 0;
    long curpos = PositionBefore(pos);
    if (curpos == 0) {
        foundPos = curpos;
        return ch;
    }

    while (true) {
        ch = GetCharAt(curpos);
        if (ch == wxT('\t') || ch == wxT(' ') || ch == wxT('\r') || ch == wxT('\v') || ch == wxT('\n')) {
            // if the caller is interested in whitespaces,
            // simply return it
            if (wantWhitespace) {
                foundPos = curpos;
                return ch;
            }

            long tmpPos = curpos;
            curpos = PositionBefore(curpos);
            if (curpos == 0 && tmpPos == curpos)
                break;
        } else {
            foundPos = curpos;
            return ch;
        }
    }
    foundPos = -1;
    return ch;
}

wxChar clEditor::NextChar(const int& pos, int& foundPos)
{
    wxChar ch = 0;
    long nextpos = pos;
    while (true) {

        if (nextpos >= GetLength())
            break;

        ch = GetCharAt(nextpos);
        if (ch == wxT('\t') || ch == wxT(' ') || ch == wxT('\r') || ch == wxT('\v') || ch == wxT('\n')) {
            nextpos = PositionAfter(nextpos);
            continue;
        } else {
            foundPos = nextpos;
            return ch;
        }
    }
    foundPos = -1;
    return ch;
}

int clEditor::FindString(const wxString& str, int flags, const bool down, long pos)
{
    // initialize direction
    if (down) {
        SetTargetStart(pos);
        SetTargetEnd(GetLength());
    } else {
        SetTargetStart(pos);
        SetTargetEnd(0);
    }
    SetSearchFlags(flags);

    // search string
    int _pos = SearchInTarget(str);
    if (_pos >= 0)
        return _pos;
    else
        return -1;
}

bool clEditor::MatchBraceBack(const wxChar& chCloseBrace, const long& pos, long& matchedPos)
{
    if (pos <= 0)
        return false;

    wxChar chOpenBrace;

    switch (chCloseBrace) {
    case '}':
        chOpenBrace = '{';
        break;
    case ')':
        chOpenBrace = '(';
        break;
    case ']':
        chOpenBrace = '[';
        break;
    case '>':
        chOpenBrace = '<';
        break;
    default:
        return false;
    }

    long nPrevPos = pos;
    wxChar ch;
    int depth = 1;

    // We go backward
    while (true) {
        if (nPrevPos == 0)
            break;
        nPrevPos = PositionBefore(nPrevPos);

        // Make sure we are not in a comment
        if (m_context->IsCommentOrString(nPrevPos))
            continue;

        ch = GetCharAt(nPrevPos);
        if (ch == chOpenBrace) {
            // Dec the depth level
            depth--;
            if (depth == 0) {
                matchedPos = nPrevPos;
                return true;
            }
        } else if (ch == chCloseBrace) {
            // Inc depth level
            depth++;
        }
    }
    return false;
}

void clEditor::RecalcHorizontalScrollbar()
{
    if (m_autoAdjustHScrollbarWidth) {
        clSTCHelper::UpdateScrollbarWidth(this, m_default_text_width);
    }
}

//--------------------------------------------------------
// Brace match
//--------------------------------------------------------

bool clEditor::IsCloseBrace(int position)
{
    return GetCharAt(position) == '}' || GetCharAt(position) == ']' || GetCharAt(position) == ')';
}

bool clEditor::IsOpenBrace(int position)
{
    return GetCharAt(position) == '{' || GetCharAt(position) == '[' || GetCharAt(position) == '(';
}

void clEditor::MatchBraceAndSelect(bool selRegion)
{
    // Get current position
    long pos = GetCurrentPos();

    if (IsOpenBrace(pos) && !m_context->IsCommentOrString(pos)) {
        BraceMatch(selRegion);
        return;
    }

    if (IsOpenBrace(PositionBefore(pos)) && !m_context->IsCommentOrString(PositionBefore(pos))) {
        SetCurrentPos(PositionBefore(pos));
        BraceMatch(selRegion);
        return;
    }

    if (IsCloseBrace(pos) && !m_context->IsCommentOrString(pos)) {
        BraceMatch(selRegion);
        return;
    }

    if (IsCloseBrace(PositionBefore(pos)) && !m_context->IsCommentOrString(PositionBefore(pos))) {
        SetCurrentPos(PositionBefore(pos));
        BraceMatch(selRegion);
        return;
    }
}

void clEditor::BraceMatch(long pos)
{
    // Check if we have a match
    m_hasBraceHighlight = true; // it can be good or bad highlight
    int indentCol = 0;
    long endPos = wxStyledTextCtrl::BraceMatch(pos);
    if (endPos != wxSTC_INVALID_POSITION) {
        wxStyledTextCtrl::BraceHighlight(pos, endPos);
#ifdef __WXMSW__
        Refresh();
#endif
        if (GetIndentationGuides() != 0 && GetIndent() > 0) {
            // Highlight indent guide if exist
            indentCol =
                std::min(GetLineIndentation(LineFromPosition(pos)), GetLineIndentation(LineFromPosition(endPos)));
            indentCol /= GetIndent();
            indentCol *= GetIndent(); // round down to nearest indentation guide column
            SetHighlightGuide(GetLineIndentation(LineFromPosition(pos)));
        }
    } else {
        wxStyledTextCtrl::BraceBadLight(pos);
    }
    SetHighlightGuide(indentCol);
}

void clEditor::BraceMatch(bool bSelRegion)
{
    // Check if we have a match
    long endPos = wxStyledTextCtrl::BraceMatch(GetCurrentPos());
    if (endPos != wxSTC_INVALID_POSITION) {
        // Highlight indent guide if exist
        long startPos = GetCurrentPos();
        if (bSelRegion) {
            // Select the range
            if (endPos > startPos) {
                SetSelectionEnd(PositionAfter(endPos));
                SetSelectionStart(startPos);
            } else {
                SetSelectionEnd(PositionAfter(startPos));
                SetSelectionStart(endPos);
            }
        } else {
            SetSelectionEnd(endPos);
            SetSelectionStart(endPos);
            SetCurrentPos(endPos);
        }
        EnsureCaretVisible();
    }
}

void clEditor::SetActive()
{
    // ensure that the top level window parent of this editor is 'Raised'
    bool raise(true);
#ifdef __WXGTK__
    // On Wayland and gtk+3.22, raise not only fails, it hangs the subsequent DnD call. See
    // http://trac.wxwidgets.org/ticket/17853
    raise = !clMainFrame::Get()->GetIsWaylandSession();
#endif
    DoUpdateTLWTitle(raise);

    SetFocus();
    SetSTCFocus(true);

    m_context->SetActive();

    wxStyledTextEvent dummy;
    OnSciUpdateUI(dummy);
}

bool clEditor::FindAndSelect(const wxString& _pattern, const wxString& name)
{
    return DoFindAndSelect(_pattern, name, 0, NavMgr::Get());
}

void clEditor::FindAndSelectV(const wxString& _pattern,
                              const wxString& name,
                              int pos /*=0*/,
                              NavMgr* WXUNUSED(unused)) // Similar but returns void, so can be async
{
    // Use CallAfter() here. With wxGTK-3.1 (perhaps due to its scintilla update) if the file wasn't already loaded,
    // EnsureVisible() is called too early and fails
    wxArrayString strings; // CallAfter can only cope with 2 parameters, so combine the wxStrings
    ClearSelections();
    strings.Add(_pattern);
    strings.Add(name);
    CallAfter(&clEditor::DoFindAndSelectV, strings, pos);
}

void clEditor::DoFindAndSelectV(const wxArrayString& strings, int pos) // Called with CallAfter()
{
    wxCHECK_RET(strings.Count() == 2, "Unexpected number of wxStrings supplied");
    wxString _pattern(strings.Item(0));
    wxString name(strings.Item(1));
    DoFindAndSelect(_pattern, name, pos, NavMgr::Get());
}

//----------------------------------------------
// Folds
//----------------------------------------------
void clEditor::ToggleCurrentFold()
{
    int line = GetCurrentLine();
    if (line >= 0) {
        DoToggleFold(line, "...");
        if (GetLineVisible(line) == false) {
            // the caret line is hidden, make sure the caret is visible
            while (line >= 0) {
                if ((GetFoldLevel(line) & wxSTC_FOLDLEVELHEADERFLAG) && GetLineVisible(line)) {
                    SetCaretAt(PositionFromLine(line));
                    break;
                }
                line--;
            }
        }
    }
}

void clEditor::DoRecursivelyExpandFolds(bool expand, int startline, int endline)
{
    for (int line = startline; line < endline; ++line) {
        if (GetFoldLevel(line) & wxSTC_FOLDLEVELHEADERFLAG) {
            int BottomOfFold = GetLastChild(line, -1);

            if (expand) {
                // Expand this fold
                SetFoldExpanded(line, true);
                ShowLines(line + 1, BottomOfFold);
                // Recursively do any contained child folds
                DoRecursivelyExpandFolds(expand, line + 1, BottomOfFold);
            } else {
                DoRecursivelyExpandFolds(expand, line + 1, BottomOfFold);
                // Hide this fold
                SetFoldExpanded(line, false);
                HideLines(line + 1, BottomOfFold);
            }

            line = BottomOfFold; // Now skip over the fold we've just dealt with, ready for any later siblings
        }
    }
}

void clEditor::ToggleAllFoldsInSelection()
{
    int selStart = GetSelectionStart();
    int selEnd = GetSelectionEnd();
    if (selStart == selEnd) {
        return; // No selection. UpdateUI prevents this from the menu, but not from an accelerator
    }

    int startline = LineFromPos(selStart);
    int endline = LineFromPos(selEnd);
    if (startline == endline) {
        DoToggleFold(startline, "..."); // For a single-line selection just toggle
        return;
    }
    if (startline > endline) {
        wxSwap(startline, endline);
    }

    // First see if there are any folded lines in the selection. If there are, we'll be in 'unfold' mode
    bool expanding(false);
    for (int line = startline; line < endline;
         ++line) { // not <=. If only the last line of the sel is folded it's unlikely that the user meant it
        if (!GetLineVisible(line)) {
            expanding = true;
            break;
        }
    }

    for (int line = startline; line < endline; ++line) {
        if (!(GetFoldLevel(line) & wxSTC_FOLDLEVELHEADERFLAG)) {
            continue;
        }
        int BottomOfFold = GetLastChild(line, -1);
        if (BottomOfFold > (endline + 1)) { // GetLastChild() seems to be 1-based, not zero-based. Without the +1, a
                                            // } at endline will be considered outside the selection
            continue;                       // This fold continues past the end of the selection
        }
        DoRecursivelyExpandFolds(expanding, line, BottomOfFold);
        line = BottomOfFold;
    }

    if (!expanding) {
        // The caret will (surely) be inside the selection, and unless it was on the first line or an unfolded one,
        // it'll now be hidden
        // If so place it at the top, which will be visible. Unfortunately SetCaretAt() destroys the selection,
        // and I can't find a way to preserve/reinstate it while still setting the caret
        int caretline = LineFromPos(GetCurrentPos());
        if (!GetLineVisible(caretline)) {
            SetCaretAt(selStart);
        }
    }
}

// If the cursor is on/in/below an open fold, collapse all. Otherwise expand all
void clEditor::FoldAll()
{
    // >(0,-1);  SciTE did this here, but it doesn't seem to accomplish anything

    // First find the current fold-point, and ask it whether or not it's folded
    int lineSeek = GetCurrentLine();
    while (true) {
        if (GetFoldLevel(lineSeek) & wxSTC_FOLDLEVELHEADERFLAG)
            break;
        int parentline = GetFoldParent(lineSeek); // See if we're inside a fold area
        if (parentline >= 0) {
            lineSeek = parentline;
            break;
        } else
            lineSeek--; // Must have been between folds
        if (lineSeek < 0)
            return;
    }
    bool expanded = GetFoldExpanded(lineSeek);

    int maxLine = GetLineCount();

    // Some files, especially headers with #ifndef FOO_H, will collapse into one big fold
    // So, if we're collapsing, skip any all-encompassing top level fold
    bool SkipTopFold = false;
    if (expanded) {
        int topline = 0;
        while (!(GetFoldLevel(topline) & wxSTC_FOLDLEVELHEADERFLAG)) {
            // This line wasn't a fold-point, so inc until we find one
            if (++topline >= maxLine)
                return;
        }
        int BottomOfFold = GetLastChild(topline, -1);
        if (BottomOfFold >= maxLine || BottomOfFold == -1)
            return;
        // We've found the bottom of the topmost fold-point. See if there's another fold below it
        ++BottomOfFold;
        while (!(GetFoldLevel(BottomOfFold) & wxSTC_FOLDLEVELHEADERFLAG)) {
            if (++BottomOfFold >= maxLine) {
                // If we're here, the top fold must encompass the whole file, so set the flag
                SkipTopFold = true;
                break;
            }
        }
    }

    // Now go through the whole document, toggling folds that match the original one's level if we're collapsing
    // or all collapsed folds if we're expanding (so that internal folds get expanded too).
    // The (level & wxSTC_FOLDLEVELHEADERFLAG) means "If this level is a Fold start"
    // (level & wxSTC_FOLDLEVELNUMBERMASK) returns a value for the 'indent' of the fold.
    // This starts at wxSTC_FOLDLEVELBASE==1024. A sub fold-point == 1025, a subsub 1026...
    for (int line = 0; line < maxLine; line++) {
        int level = GetFoldLevel(line);
        // If we're skipping an all-encompassing fold, we use wxSTC_FOLDLEVELBASE+1
        if ((level & wxSTC_FOLDLEVELHEADERFLAG) &&
            (expanded ? ((level & wxSTC_FOLDLEVELNUMBERMASK) == (wxSTC_FOLDLEVELBASE + SkipTopFold))
                      : ((level & wxSTC_FOLDLEVELNUMBERMASK) >= wxSTC_FOLDLEVELBASE))) {
            if (GetFoldExpanded(line) == expanded)
                DoToggleFold(line, "...");
        }
    }

    // make sure the caret is visible. If it was hidden, place it at the first visible line
    int curpos = GetCurrentPos();
    if (curpos != wxNOT_FOUND) {
        int curline = LineFromPosition(curpos);
        if (curline != wxNOT_FOUND && GetLineVisible(curline) == false) {
            // the caret line is hidden, make sure the caret is visible
            while (curline >= 0) {
                if ((GetFoldLevel(curline) & wxSTC_FOLDLEVELHEADERFLAG) && GetLineVisible(curline)) {
                    SetCaretAt(PositionFromLine(curline));
                    break;
                }
                curline--;
            }
        }
    }
}

// Toggle all the highest-level folds in the selection i.e. if the selection contains folds of level 3, 4 and 5,
// toggle all the level 3 ones
void clEditor::ToggleTopmostFoldsInSelection()
{
    int selStart = GetSelectionStart();
    int selEnd = GetSelectionEnd();
    if (selStart == selEnd) {
        return; // No selection. UpdateUI prevents this from the menu, but not from an accelerator
    }

    int startline = LineFromPos(selStart);
    int endline = LineFromPos(selEnd);
    if (startline == endline) {
        DoToggleFold(startline, "..."); // For a single-line selection just toggle
        return;
    }
    if (startline > endline) {
        wxSwap(startline, endline);
    }

    // Go thru the selection to find the topmost contained fold level. Also ask the first one of this level if it's
    // folded
    int toplevel(wxSTC_FOLDLEVELNUMBERMASK);
    bool expanded(true);
    for (int line = startline; line < endline;
         ++line) { // not <=. If only the last line of the sel is folded it's unlikely that the user meant it
        if (!GetLineVisible(line)) {
            break;
        }
        if (GetFoldLevel(line) & wxSTC_FOLDLEVELHEADERFLAG) {
            int level = GetFoldLevel(line) & wxSTC_FOLDLEVELNUMBERMASK;
            if (level < toplevel) {
                toplevel = level;
                expanded = GetFoldExpanded(line);
            }
        }
    }
    if (toplevel == wxSTC_FOLDLEVELNUMBERMASK) { // No fold found
        return;
    }

    for (int line = startline; line < endline; ++line) {
        if (GetFoldLevel(line) & wxSTC_FOLDLEVELHEADERFLAG) {
            if ((GetFoldLevel(line) & wxSTC_FOLDLEVELNUMBERMASK) == toplevel && GetFoldExpanded(line) == expanded) {
                DoToggleFold(line, "...");
            }
        }
    }

    // make sure the caret is visible. If it was hidden, place it at the first visible line
    int curpos = GetCurrentPos();
    if (expanded && curpos != wxNOT_FOUND) {
        int curline = LineFromPosition(curpos);
        if (curline != wxNOT_FOUND && GetLineVisible(curline) == false) {
            // the caret line is hidden, make sure the caret is visible
            while (curline >= 0) {
                if ((GetFoldLevel(curline) & wxSTC_FOLDLEVELHEADERFLAG) && GetLineVisible(curline)) {
                    SetCaretAt(PositionFromLine(curline));
                    break;
                }
                curline--;
            }
        }
    }
}

void clEditor::StoreCollapsedFoldsToArray(clEditorStateLocker::VecInt_t& folds) const
{
    clEditorStateLocker::SerializeFolds(const_cast<wxStyledTextCtrl*>(static_cast<const wxStyledTextCtrl*>(this)),
                                        folds);
}

void clEditor::LoadCollapsedFoldsFromArray(const clEditorStateLocker::VecInt_t& folds)
{
    clEditorStateLocker::ApplyFolds(GetCtrl(), folds);
}

//----------------------------------------------
// Bookmarks
//----------------------------------------------
void clEditor::AddMarker()
{
    int nPos = GetCurrentPos();
    int nLine = LineFromPosition(nPos);
    int nBits = MarkerGet(nLine);
    if (nBits & mmt_standard_bookmarks) {
        clDEBUG() << "Marker already exists in" << GetFileName() << ":" << nLine;
        return;
    }
    MarkerAdd(nLine, GetActiveBookmarkType());

    // Notify about marker changes
    NotifyMarkerChanged(nLine);
}

void clEditor::DelMarker()
{
    int nPos = GetCurrentPos();
    int nLine = LineFromPosition(nPos);
    for (int i = smt_FIRST_BMK_TYPE; i < smt_LAST_BMK_TYPE; ++i) {
        MarkerDelete(nLine, i);
        // Notify about marker changes
        NotifyMarkerChanged(nLine);
    }
}

void clEditor::ToggleMarker()
{
    // Add/Remove marker
    if (!LineIsMarked(mmt_standard_bookmarks)) {
        AddMarker();
    } else {
        while (LineIsMarked(mmt_standard_bookmarks)) {
            DelMarker();
        }
    }
}

bool clEditor::LineIsMarked(enum marker_mask_type mask)
{
    int nPos = GetCurrentPos();
    int nLine = LineFromPosition(nPos);
    int nBits = MarkerGet(nLine);
    // 'mask' is a bitmap representing a bookmark, or a type of breakpt, or...
    return (nBits & mask ? true : false);
}

void clEditor::StoreMarkersToArray(wxArrayString& bookmarks)
{
    clEditorStateLocker::SerializeBookmarks(GetCtrl(), bookmarks);
}

void clEditor::LoadMarkersFromArray(const wxArrayString& bookmarks)
{
    clEditorStateLocker::ApplyBookmarks(GetCtrl(), bookmarks);
}

void clEditor::DelAllMarkers(int which_type)
{
    // Delete all relevant markers from the view
    // If 0, delete just the currently active type, -1 delete them all.
    // Otherwise just the specified type, which will usually be the 'find' bookmark
    if (which_type > 0) {
        MarkerDeleteAll(which_type);
    } else if (which_type == 0) {
        MarkerDeleteAll(GetActiveBookmarkType());
    } else {
        for (size_t bmt = smt_FIRST_BMK_TYPE; bmt <= smt_LAST_BMK_TYPE; ++bmt) {
            MarkerDeleteAll(bmt);
        }
    }

    // delete other markers as well
    SetIndicatorCurrent(1);
    IndicatorClearRange(0, GetLength());

    SetIndicatorCurrent(INDICATOR_WORD_HIGHLIGHT);
    IndicatorClearRange(0, GetLength());

    SetIndicatorCurrent(INDICATOR_HYPERLINK);
    IndicatorClearRange(0, GetLength());

    SetIndicatorCurrent(INDICATOR_DEBUGGER);
    IndicatorClearRange(0, GetLength());

    SetIndicatorCurrent(INDICATOR_FIND_BAR_WORD_HIGHLIGHT);
    IndicatorClearRange(0, GetLength());

    // Notify about marker changes
    NotifyMarkerChanged();
}

size_t clEditor::GetFindMarkers(std::vector<std::pair<int, wxString>>& bookmarksVector)
{
    int nPos = 0;
    int nFoundLine = LineFromPosition(nPos);
    while (nFoundLine < GetLineCount()) {
        nFoundLine = MarkerNext(nFoundLine, GetActiveBookmarkMask());
        if (nFoundLine == wxNOT_FOUND) {
            break;
        }
        wxString snippet = GetLine(nFoundLine);
        snippet.Trim().Trim(false);
        if (!snippet.IsEmpty()) {
            snippet = snippet.Mid(0, snippet.size() > 40 ? 40 : snippet.size());
            if (snippet.size() == 40) {
                snippet << "...";
            }
        }
        bookmarksVector.push_back({ nFoundLine + 1, snippet });
        ++nFoundLine;
    }
    return bookmarksVector.size();
}

void clEditor::FindNextMarker()
{
    int nPos = GetCurrentPos();
    int nLine = LineFromPosition(nPos);
    int nFoundLine = MarkerNext(nLine + 1, GetActiveBookmarkMask());
    if (nFoundLine >= 0) {
        // mark this place before jumping to next marker
        CenterLine(nFoundLine);
    } else {
        // We reached the last marker, try again from top
        nLine = LineFromPosition(0);
        nFoundLine = MarkerNext(nLine, GetActiveBookmarkMask());
        if (nFoundLine >= 0) {
            CenterLine(nFoundLine);
        }
    }
    if (nFoundLine >= 0) {
        EnsureVisible(nFoundLine);
        EnsureCaretVisible();
    }
}

void clEditor::FindPrevMarker()
{
    int nPos = GetCurrentPos();
    int nLine = LineFromPosition(nPos);
    int mask = GetActiveBookmarkMask();
    int nFoundLine = MarkerPrevious(nLine - 1, mask);
    if (nFoundLine >= 0) {
        CenterLine(nFoundLine);
    } else {
        // We reached first marker, try again from button
        int nFileSize = GetLength();
        nLine = LineFromPosition(nFileSize);
        nFoundLine = MarkerPrevious(nLine, mask);
        if (nFoundLine >= 0) {
            CenterLine(nFoundLine);
        }
    }
    if (nFoundLine >= 0) {
        EnsureVisible(nFoundLine);
        EnsureCaretVisible();
    }
}

int clEditor::GetActiveBookmarkType() const
{
    if (IsFindBookmarksActive()) {
        return smt_find_bookmark;

    } else {
        return BookmarkManager::Get().GetActiveBookmarkType();
    }
}

enum marker_mask_type clEditor::GetActiveBookmarkMask() const
{
    wxASSERT(1 << smt_find_bookmark == mmt_find_bookmark);
    if (IsFindBookmarksActive()) {
        return mmt_find_bookmark;
    } else {
        return (marker_mask_type)(1 << BookmarkManager::Get().GetActiveBookmarkType());
    }
}

wxString clEditor::GetBookmarkLabel(sci_marker_types type)
{
    wxCHECK_MSG(type >= smt_FIRST_BMK_TYPE && type <= smt_LAST_BMK_TYPE, "", "Invalid marker type");
    wxString label = BookmarkManager::Get().GetMarkerLabel(type);
    if (label.empty()) {
        label = wxString::Format(_("Type %i bookmark"), type - smt_FIRST_BMK_TYPE + 1);
    }

    return label;
}

void clEditor::OnChangeActiveBookmarkType(wxCommandEvent& event)
{
    int requested = event.GetId() - XRCID("BookmarkTypes[start]");
    BookmarkManager::Get().SetActiveBookmarkType(requested + smt_FIRST_BMK_TYPE - 1);
    if ((requested + smt_FIRST_BMK_TYPE - 1) != smt_find_bookmark) {
        SetFindBookmarksActive(false);
    }

    clMainFrame::Get()->SelectBestEnvSet(); // Updates the statusbar display
}

void clEditor::GetBookmarkTooltip(int lineno, wxString& tip, wxString& title)
{
    title << "### " << _("Bookmarks");
    // If we've arrived here we know there's a bookmark on the line; however we don't know which type(s)
    // If multiple, list each, with the visible one first
    int linebits = MarkerGet(lineno);
    if (linebits & GetActiveBookmarkMask()) {
        tip << GetBookmarkLabel((sci_marker_types)GetActiveBookmarkType());
    }

    for (int bmt = smt_FIRST_BMK_TYPE; bmt <= smt_LAST_BMK_TYPE; ++bmt) {
        if (bmt != GetActiveBookmarkType()) {
            if (linebits & (1 << bmt)) {
                if (!tip.empty()) {
                    tip << "\n";
                }
                tip << GetBookmarkLabel((sci_marker_types)bmt);
            }
        }
    }
}

wxFontEncoding clEditor::DetectEncoding(const wxString& filename)
{
    wxFontEncoding encoding = GetOptions()->GetFileFontEncoding();
#if defined(USE_UCHARDET)
    wxFile file(filename);
    if (!file.IsOpened())
        return encoding;

    size_t size = file.Length();
    if (size == 0) {
        file.Close();
        return encoding;
    }

    wxByte* buffer = (wxByte*)malloc(sizeof(wxByte) * (size + 4));
    if (!buffer) {
        file.Close();
        return encoding;
    }
    buffer[size + 0] = 0;
    buffer[size + 1] = 0;
    buffer[size + 2] = 0;
    buffer[size + 3] = 0;

    size_t readBytes = file.Read((void*)buffer, size);
    bool result = false;
    if (readBytes > 0) {
        uchardet_t ud = uchardet_new();
        if (0 == uchardet_handle_data(ud, (const char*)buffer, readBytes)) {
            uchardet_data_end(ud);
            wxString charset(uchardet_get_charset(ud));
            charset.MakeUpper();
            if (charset.find("UTF-8") != wxString::npos) {
                encoding = wxFONTENCODING_UTF8;
            } else if (charset.find("GB18030") != wxString::npos) {
                encoding = wxFONTENCODING_GB2312;
            } else if (charset.find("BIG5") != wxString::npos) {
                encoding = wxFONTENCODING_BIG5;
            } else if (charset.find("EUC-JP") != wxString::npos) {
                encoding = wxFONTENCODING_EUC_JP;
            } else if (charset.find("EUC-KR") != wxString::npos) {
                encoding = wxFONTENCODING_EUC_KR;
            } else if (charset.find("WINDOWS-1252") != wxString::npos) {
                encoding = wxFONTENCODING_CP1252;
            } else if (charset.find("WINDOWS-1255") != wxString::npos) {
                encoding = wxFONTENCODING_CP1255;
            } else if (charset.find("ISO-8859-8") != wxString::npos) {
                encoding = wxFONTENCODING_ISO8859_8;
            } else if (charset.find("SHIFT_JIS") != wxString::npos) {
                encoding = wxFONTENCODING_SHIFT_JIS;
            }
        }
        uchardet_delete(ud);
    }
    file.Close();
    free(buffer);
#endif
    return encoding;
}

void clEditor::DoUpdateLineNumbers(bool relative_numbers, bool force)
{
    auto state = EditorViewState::From(this);
    if (state == m_editorState && !force) {
        return;
    }

    if (!GetOptions()->IsLineNumberHighlightCurrent() && !force)
        return;

    int linesOnScreen = LinesOnScreen();
    int current_line = GetCurrentLine();

    std::vector<int> lines;
    lines.reserve(linesOnScreen);

    // GetFirstVisibleLine() does not report the correct visible line when
    // there are folded lines above it, so we calculate it manually
    int lastLine = GetNumberOfLines();
    int counter = 0;

    // this should return the real first visible line number
    int first_visible_line = DocLineFromVisible(GetFirstVisibleLine());

    wxString line_text;
    line_text.reserve(100);

    // now calculate the last line that we want draw
    int curline = first_visible_line;
    counter = 0;
    while ((counter < linesOnScreen + 1) && curline <= lastLine) {
        if (GetLineVisible(curline)) {
            counter++;
            lines.push_back(curline);
        }
        curline++;
    }

    // first: the real line number
    // second: line number to display in the margin
    // when relative_numbers is TRUE, the values are
    // different, otherwise, they are identical
    std::vector<std::pair<int, int>> lines_to_draw;

    // update the lines to relative if needed
    // this method takes folded / annotations lines into consideration
    //
    // 10 | ..
    // 11 + folded line
    // 15 | ..
    // 16 | <== current line
    // 17 + folded line
    // 20 | ..
    //
    // Becomes:
    //
    // ||
    // \/
    //
    // 6  | ..
    // 5  + folded line
    // 1  | ..
    // 16 | <== current line
    // 1  + folded line
    // 4  | ..
    lines_to_draw.reserve(lines.size());
    for (int line : lines) {
        if (relative_numbers) {
            if (line < current_line) {
                lines_to_draw.push_back({ line, current_line - line });
            } else if (line == current_line) {
                // nothing to be done here
                lines_to_draw.push_back({ line, line + 1 });
            } else {
                lines_to_draw.push_back({ line, line - current_line });
            }
        } else {
            lines_to_draw.push_back({ line, line + 1 });
        }
    }

    // set the line numbers, taking hidden lines into consideration
    for (auto& [line_number, line_to_render] : lines_to_draw) {
        line_text.Printf(wxT("%d"), line_to_render);
        MarginSetText(line_number, line_text);

        bool is_current_line = (line_number == current_line);
        if (m_trackChanges) {
            if (auto iter = m_modifiedLines.find(line_number); iter != m_modifiedLines.end()) {
                const auto& line_status = iter->second;
                if (line_status == LINE_MODIFIED) {
                    MarginSetStyle(line_number, is_current_line ? STYLE_CURRENT_LINE_MODIFIED : STYLE_MODIFIED_LINE);
                } else if (line_status == LINE_SAVED) {
                    MarginSetStyle(line_number, is_current_line ? STYLE_CURRENT_LINE_SAVED : STYLE_SAVED_LINE);
                } else {
                    MarginSetStyle(line_number, is_current_line ? STYLE_CURRENT_LINE : STYLE_NORMAL_LINE);
                }
            } else {
                // normal line
                MarginSetStyle(line_number, is_current_line ? STYLE_CURRENT_LINE : STYLE_NORMAL_LINE);
            }
        } else {
            MarginSetStyle(line_number, is_current_line ? STYLE_CURRENT_LINE : STYLE_NORMAL_LINE);
        }
    }
}

void clEditor::UpdateLineNumbers(bool force)
{
    OptionsConfigPtr c = GetOptions();
    if (!c->GetDisplayLineNumbers() || !c->IsLineNumberHighlightCurrent()) {
        return;
    }
    DoUpdateLineNumbers(c->GetRelativeLineNumbers(), force);
}

void clEditor::OpenFile()
{
    wxBusyCursor bc;
    wxWindowUpdateLocker locker(this);
    SetReloadingFile(true);

    DoCancelCalltip();
    GetFunctionTip()->Deactivate();

    if (m_fileName.GetFullPath().IsEmpty() == true || !m_fileName.FileExists()) {
        SetEOLMode(GetEOLByOS());
        SetReloadingFile(false);
        return;
    }

    // State locker (on dtor it restores: bookmarks, current line, breakpoints and folds)
    clEditorStateLocker stateLocker(GetCtrl());

    int lineNumber = GetCurrentLine();
    m_mgr->GetStatusBar()->SetMessage(_("Loading file..."));

    wxString text;

    // Read the file we currently support:
    // BOM, Auto-Detect encoding & User defined encoding
    m_fileBom.Clear();
    ReadFileWithConversion(m_fileName.GetFullPath(), text, DetectEncoding(m_fileName.GetFullPath()), &m_fileBom);

    SetText(text);

    m_modifyTime = GetFileLastModifiedTime();

    SetSavePoint();
    EmptyUndoBuffer();
    GetCommandsProcessor().Reset();

    // Update the editor properties
    UpdateOptions();
    UpdateLineNumberMarginWidth();
    UpdateColours();
    SetEOL();

    int doclen = GetLength();
    int lastLine = LineFromPosition(doclen);
    lineNumber > lastLine ? lineNumber = lastLine : lineNumber;

    SetEnsureCaretIsVisible(PositionFromLine(lineNumber));

    // mark read only files
    clMainFrame::Get()->GetMainBook()->MarkEditorReadOnly(this);
    SetReloadingFile(false);

    // Notify that a file has been loaded into the editor
    clCommandEvent fileLoadedEvent(wxEVT_FILE_LOADED);
    fileLoadedEvent.SetFileName(FileUtils::RealPath(GetFileName().GetFullPath()));
    EventNotifier::Get()->AddPendingEvent(fileLoadedEvent);

    SetProperty(wxT("lexer.cpp.track.preprocessor"), wxT("0"));
    SetProperty(wxT("lexer.cpp.update.preprocessor"), wxT("0"));
    m_mgr->GetStatusBar()->SetMessage(_("Ready"));
    CallAfter(&clEditor::SetProperties);
}

void clEditor::SetEditorText(const wxString& text)
{
    wxWindowUpdateLocker locker(this);
    SetText(text);

    // remove breakpoints belongs to this file
    DelAllBreakpointMarkers();
}

void clEditor::CreateRemote(const wxString& local_path, const wxString& remote_path, const wxString& ssh_account)
{
    SetFileName(local_path);
    SetProject(wxEmptyString);
    SetSyntaxHighlight(false);
    // mark this file as remote by setting a remote data
    IEditor::SetClientData("sftp", std::make_unique<SFTPClientData>(local_path, remote_path, ssh_account));
    OpenFile();
}

void clEditor::Create(const wxString& project, const wxFileName& fileName)
{
    // set the file name
    SetFileName(fileName);
    // set the project name
    SetProject(project);
    // let the editor choose the syntax highlight to use according to file extension
    // and set the editor properties to default
    SetSyntaxHighlight(false); // Dont call 'UpdateColors' it is called in 'OpenFile'
    // reload the file from disk
    OpenFile();
}

void clEditor::InsertTextWithIndentation(const wxString& text, int lineno)
{
    wxString textTag = FormatTextKeepIndent(text, PositionFromLine(lineno));
    InsertText(PositionFromLine(lineno), textTag);
}

wxString clEditor::FormatTextKeepIndent(const wxString& text, int pos, size_t flags)
{
    // keep the page idnetation level
    wxString textToInsert(text);
    wxString indentBlock;

    int indentSize = 0;
    int indent = 0;

    if (flags & Format_Text_Indent_Prev_Line) {
        indentSize = GetIndent();
        int foldLevel = (GetFoldLevel(LineFromPosition(pos)) & wxSTC_FOLDLEVELNUMBERMASK) - wxSTC_FOLDLEVELBASE;
        indent = foldLevel * indentSize;

    } else {
        indentSize = GetIndent();
        indent = GetLineIndentation(LineFromPosition(pos));
    }

    if (GetUseTabs()) {
        if (indentSize)
            indent = indent / indentSize;

        for (int i = 0; i < indent; i++) {
            indentBlock << wxT("\t");
        }
    } else {
        for (int i = 0; i < indent; i++) {
            indentBlock << wxT(" ");
        }
    }

    wxString eol = GetEolString();
    textToInsert.Replace(wxT("\r"), wxT("\n"));
    wxStringTokenizerMode tokenizerMode = (flags & Format_Text_Save_Empty_Lines) ? wxTOKEN_RET_EMPTY : wxTOKEN_STRTOK;
    wxArrayString lines = wxStringTokenize(textToInsert, wxT("\n"), tokenizerMode);

    textToInsert.Clear();
    for (size_t i = 0; i < lines.GetCount(); i++) {
        textToInsert << indentBlock;
        textToInsert << lines.Item(i) << eol;
    }
    return textToInsert;
}

void clEditor::OnContextMenu(wxContextMenuEvent& event)
{
    wxString selectText = GetSelectedText();
    wxPoint pt = event.GetPosition();
    if (pt != wxDefaultPosition) { // Analyze position only for mouse-originated events
        wxPoint clientPt = ScreenToClient(pt);

        // If the right-click is in the margin, provide a different context menu: bookmarks/breakpts
        int margin = 0;
        for (int n = 0; n < LAST_MARGIN_ID;
             ++n) { // Assume a click anywhere to the left of the fold margin is for markers
            margin += GetMarginWidth(n);
        }

        if (clientPt.x < margin) {
            GotoPos(PositionFromPoint(clientPt));
            DoBreakptContextMenu(clientPt);
            return;
        }

        int closePos = PositionFromPointClose(clientPt.x, clientPt.y);
        if (closePos != wxNOT_FOUND) {
            if (!selectText.IsEmpty()) {
                // If the selection text is placed under the cursor,
                // keep it selected, else, unselect the text
                // and place the caret to be under cursor
                int selStart = GetSelectionStart();
                int selEnd = GetSelectionEnd();
                if (closePos < selStart || closePos > selEnd) {
                    // cursor is not over the selected text, unselect and re-position caret
                    SetCaretAt(closePos);
                }
            } else {
                // no selection, just place the caret
                SetCaretAt(closePos);
            }
        }
    }
    // Let the plugins handle this event first
    wxCommandEvent contextMenuEvent(wxEVT_CMD_EDITOR_CONTEXT_MENU, GetId());
    contextMenuEvent.SetEventObject(this);
    if (EventNotifier::Get()->ProcessEvent(contextMenuEvent))
        return;

    wxMenu* menu = m_context->GetMenu();
    if (!menu)
        return;

    // Let the context add it dynamic content
    m_context->AddMenuDynamicContent(menu);

    // add the debugger (if currently running) to add its dynamic content
    IDebugger* debugger = DebuggerMgr::Get().GetActiveDebugger();
    if (debugger && debugger->IsRunning()) {
        AddDebuggerContextMenu(menu);
    }

    // turn the popupIsOn value to avoid annoying
    // calltips from firing while our menu is popped
    m_popupIsOn = true;

    // Notify about menu is about to be shown
    clContextMenuEvent menuEvent(wxEVT_CONTEXT_MENU_EDITOR);
    menuEvent.SetEditor(this);
    menuEvent.SetMenu(menu);
    EventNotifier::Get()->ProcessEvent(menuEvent);

    // let the plugins hook their content
    PluginManager::Get()->HookPopupMenu(menu, MenuTypeEditor);

    // +++++------------------------------------------------------
    // if the selection is URL, offer to open it in the browser
    // +++++------------------------------------------------------
    wxString selectedText = GetSelectedText();
    if (!selectedText.IsEmpty() && !selectedText.Contains("\n")) {
        if (selectText.StartsWith("https://") || selectText.StartsWith("http://")) {
            // Offer to open the URL
            if (ID_OPEN_URL == wxNOT_FOUND) {
                ID_OPEN_URL = ::wxNewId();
            }

            wxString text;
            text << "Open: " << selectText;
            menu->PrependSeparator();
            menu->Prepend(ID_OPEN_URL, text);
            menu->Bind(wxEVT_MENU, &clEditor::OpenURL, this, ID_OPEN_URL);
        }
    }
    // +++++--------------------------
    // Popup the menu
    // +++++--------------------------
    CursorChanger cd{ this };
    PopupMenu(menu);
    wxDELETE(menu);

    m_popupIsOn = false;
    event.Skip();
}

void clEditor::OnKeyDown(wxKeyEvent& event)
{
    bool is_pos_before_whitespace = wxIsspace(SafeGetChar(PositionBefore(GetCurrentPos())));
    bool backspace_triggers_cc = TagsManagerST::Get()->GetCtagsOptions().GetFlags() & CC_BACKSPACE_TRIGGER;
    if (backspace_triggers_cc && !is_pos_before_whitespace && (event.GetKeyCode() == WXK_BACK) && !m_calltip) {
        // try to code complete
        clCodeCompletionEvent evt(wxEVT_CC_CODE_COMPLETE);
        evt.SetPosition(GetCurrentPosition());
        evt.SetInsideCommentOrString(m_context->IsCommentOrString(PositionBefore(GetCurrentPos())));
        evt.SetTriggerKind(LSP::CompletionItem::kTriggerUser);
        evt.SetFileName(GetFileName().GetFullPath());
        EventNotifier::Get()->AddPendingEvent(evt);
    }

    m_prevSelectionInfo.Clear();
    if (HasSelection()) {
        for (int i = 0; i < GetSelections(); ++i) {
            int selStart = GetSelectionNStart(i);
            int selEnd = GetSelectionNEnd(i);
            if (selEnd > selStart) {
                m_prevSelectionInfo.AddSelection(selStart, selEnd);
            } else {
                m_prevSelectionInfo.Clear();
                break;
            }
        }
        m_prevSelectionInfo.Sort();
    }

    bool escapeUsed = false; // If the quickfind bar is open we'll use an ESC to close it; but only if we've not
                             // already used it for something else

    // Hide tooltip dialog if its ON
    IDebugger* dbgr = DebuggerMgr::Get().GetActiveDebugger();
    bool dbgTipIsShown = ManagerST::Get()->GetDebuggerTip()->IsShown();
    bool keyIsControl = event.GetModifiers() == wxMOD_CONTROL;

    if (keyIsControl) {
        // Debugger tooltip is shown when clicking 'Control/CMD'
        // while the mouse is over a word
        wxPoint pt = ScreenToClient(wxGetMousePosition());
        int pos = PositionFromPointClose(pt.x, pt.y);
        if (pos != wxNOT_FOUND) {
            // try the selection first
            wxString word = GetSelectedText();
            if (word.empty()) {
                // pick the word next to the cursor
                word = GetWordAtPosition(pos, false);
            }

            if (!word.IsEmpty()) {
                clDebugEvent tipEvent(wxEVT_DBG_EXPR_TOOLTIP);
                tipEvent.SetString(word);
                if (EventNotifier::Get()->ProcessEvent(tipEvent)) {
                    return;
                }
            }
        }
    }

    if (dbgTipIsShown && !keyIsControl) {

        // If any key is pressed, but the CONTROL key hide the
        // debugger tip
        ManagerST::Get()->GetDebuggerTip()->HideDialog();

        // Destroy any floating tooltips out there
        clCommandEvent destroyEvent(wxEVT_TOOLTIP_DESTROY);
        EventNotifier::Get()->AddPendingEvent(destroyEvent);

        escapeUsed = true;

    } else if (dbgr && dbgr->IsRunning() && ManagerST::Get()->DbgCanInteract() && keyIsControl) {

        DebuggerInformation info;
        DebuggerMgr::Get().GetDebuggerInformation(dbgr->GetName(), info);

        if (info.showTooltipsOnlyWithControlKeyIsDown) {
            // CONTROL Key + Debugger is running and interactive
            // and no debugger tip is shown -> emulate "Dwell" event
            wxStyledTextEvent sciEvent;
            wxPoint pt(ScreenToClient(wxGetMousePosition()));
            sciEvent.SetPosition(PositionFromPointClose(pt.x, pt.y));

            m_context->OnDbgDwellStart(sciEvent);
        }
    }

    // let the context process it as well
    if (event.GetKeyCode() == WXK_ESCAPE) {

        // Destroy any floating tooltips out there
        clCommandEvent destroyEvent(wxEVT_TOOLTIP_DESTROY);
        EventNotifier::Get()->AddPendingEvent(destroyEvent);

        // if we are in fullscreen mode, hitting ESC will disable this
        wxFrame* mainframe = EventNotifier::Get()->TopFrame();
        if (mainframe->IsFullScreen()) {
            mainframe->ShowFullScreen(false,
                                      wxFULLSCREEN_NOMENUBAR | wxFULLSCREEN_NOTOOLBAR | wxFULLSCREEN_NOBORDER |
                                          wxFULLSCREEN_NOCAPTION);
        }

        if (GetFunctionTip()->IsActive()) {
            GetFunctionTip()->Deactivate();
            escapeUsed = true;
        }

        // If we've not already used ESC, there's a reasonable chance that the user wants to close the QuickFind bar
        if (!escapeUsed) {
            clMainFrame::Get()->GetMainBook()->ShowQuickBar(
                false); // There's no easy way to tell if it's actually showing, so just do a Close
            // In addition, if we have multiple selections, de-select them
            if (GetSelections()) {
                clEditorStateLocker editor(this);
                ClearSelections();
            }
        }
    }
    m_context->OnKeyDown(event);
}

void clEditor::OnLeftUp(wxMouseEvent& event)
{
    m_isDragging = false; // We can't still be in D'n'D, so stop disabling callticks
    DoQuickJump(event, false);

    PostCmdEvent(wxEVT_EDITOR_CLICKED);
    event.Skip();
    UpdateLineNumbers(true);
}

void clEditor::OnLeaveWindow(wxMouseEvent& event)
{
    m_hyperLinkIndicatroStart = wxNOT_FOUND;
    m_hyperLinkIndicatroEnd = wxNOT_FOUND;

    SetIndicatorCurrent(INDICATOR_HYPERLINK);
    IndicatorClearRange(0, GetLength());
    event.Skip();
}

void clEditor::OnFocusLost(wxFocusEvent& event)
{
    m_isFocused = false;
    event.Skip();
    UpdateLineNumbers(true);

    // release the tooltip
    DoCancelCalltip();
    DoCancelCodeCompletionBox();

    if (HasCapture()) {
        ReleaseMouse();
    }

    clCommandEvent focus_lost{ wxEVT_STC_LOST_FOCUS };
    EventNotifier::Get()->AddPendingEvent(focus_lost);
}

void clEditor::OnRightDown(wxMouseEvent& event)
{
    int mod = GetCodeNavModifier();
    if (event.GetModifiers() == mod && mod != wxMOD_NONE) {
        ClearSelections();
        long pos = PositionFromPointClose(event.GetX(), event.GetY());
        if (pos != wxNOT_FOUND) {
            DoSetCaretAt(pos);
        }

        clCodeCompletionEvent event(wxEVT_CC_SHOW_QUICK_NAV_MENU);
        event.SetPosition(pos);
        event.SetInsideCommentOrString(m_context->IsCommentOrString(pos));
        event.SetFileName(FileUtils::RealPath(GetFileName().GetFullPath()));
        EventNotifier::Get()->AddPendingEvent(event);

    } else {
        event.Skip();
    }
}

void clEditor::OnMotion(wxMouseEvent& event)
{
    int mod = GetCodeNavModifier();
    if (event.GetModifiers() == mod && mod != wxMOD_NONE) {

        m_hyperLinkIndicatroStart = wxNOT_FOUND;
        m_hyperLinkIndicatroEnd = wxNOT_FOUND;

        SetIndicatorCurrent(INDICATOR_HYPERLINK);
        IndicatorClearRange(0, GetLength());
        DoMarkHyperlink(event, true);
    } else {
        event.Skip();
        if (GetSTCCursor() != wxSTC_CURSORNORMAL) {
            SetSTCCursor(wxSTC_CURSORNORMAL);
        }
    }
}

void clEditor::OnLeftDown(wxMouseEvent& event)
{
    HighlightWord(false);
    wxDELETE(m_richTooltip);

    // Clear context word highlight
    SetIndicatorCurrent(INDICATOR_CONTEXT_WORD_HIGHLIGHT);
    IndicatorClearRange(0, GetLength());

    // hide completion box
    DoCancelCalltip();
    GetFunctionTip()->Deactivate();

    if (ManagerST::Get()->GetDebuggerTip()->IsShown()) {
        ManagerST::Get()->GetDebuggerTip()->HideDialog();
    }

    int mod = GetCodeNavModifier();
    if (m_hyperLinkIndicatroEnd != wxNOT_FOUND && m_hyperLinkIndicatroStart != wxNOT_FOUND &&
        event.GetModifiers() == mod && mod != wxMOD_NONE) {
        ClearSelections();
        SetCaretAt(PositionFromPointClose(event.GetX(), event.GetY()));
    }
    SetActive();

    // Destroy any floating tooltips out there
    clCommandEvent destroyEvent(wxEVT_TOOLTIP_DESTROY);
    EventNotifier::Get()->AddPendingEvent(destroyEvent);

    // Clear any messages from the status bar
    clGetManager()->GetStatusBar()->SetMessage(wxEmptyString);
    event.Skip();
}

void clEditor::OnPopupMenuUpdateUI(wxUpdateUIEvent& event)
{
    // pass it to the context
    m_context->ProcessEvent(event);
}

BrowseRecord clEditor::CreateBrowseRecord()
{
    // Remember this position before skipping to the next one
    BrowseRecord record;
    record.lineno = LineFromPosition(GetCurrentPos()); // scintilla counts from zero, while tagentry from 1
    record.filename = GetRemotePathOrLocal();
    record.project = GetProject();
    record.firstLineInView = GetFirstVisibleLine();
    record.column = GetColumn(GetCurrentPosition());
    if (IsRemoteFile()) {
        record.ssh_account = GetRemoteData()->GetAccountName();
    }
    return record;
}

void clEditor::DoBreakptContextMenu(wxPoint pt)
{
    // turn the popupIsOn value to avoid annoying
    // calltips from firing while our menu is popped
    m_popupIsOn = true;

    wxMenu menu;

    // First, add/del bookmark
    menu.Append(XRCID("toggle_bookmark"),
                LineIsMarked(mmt_standard_bookmarks) ? wxString(_("Remove Bookmark")) : wxString(_("Add Bookmark")));
    menu.Append(XRCID("removeall_bookmarks"), _("Remove All Bookmarks"));

    BookmarkManager::Get().CreateBookmarksSubmenu(&menu);
    menu.AppendSeparator();

    menu.Append(XRCID("copy_breakpoint_format"), _("Copy lldb/gdb 'set breakpoint' command to clipboard"));
    menu.AppendSeparator();

    menu.Append(XRCID("add_breakpoint"), wxString(_("Add Breakpoint")));
    menu.Append(XRCID("insert_temp_breakpoint"), wxString(_("Add a Temporary Breakpoint")));
    menu.Append(XRCID("insert_disabled_breakpoint"), wxString(_("Add a Disabled Breakpoint")));
    menu.Append(XRCID("insert_cond_breakpoint"), wxString(_("Add a Conditional Breakpoint..")));

    clDebuggerBreakpoint& bp = ManagerST::Get()->GetBreakpointsMgr()->GetBreakpoint(
        FileUtils::RealPath(GetFileName().GetFullPath()), GetCurrentLine() + 1);

    // What we show depends on whether there's already a bp here (or several)
    if (!bp.IsNull()) {

        // Disable all the "Add*" entries
        menu.Enable(XRCID("add_breakpoint"), false);
        menu.Enable(XRCID("insert_temp_breakpoint"), false);
        menu.Enable(XRCID("insert_disabled_breakpoint"), false);
        menu.Enable(XRCID("insert_cond_breakpoint"), false);
        menu.AppendSeparator();

        menu.Append(XRCID("delete_breakpoint"), wxString(_("Remove Breakpoint")));
        menu.Append(XRCID("ignore_breakpoint"), wxString(_("Ignore Breakpoint")));
        // On MSWin it often crashes the debugger to try to load-then-disable a bp
        // so don't show the menu item unless the debugger is running *** Hmm, that was written about 4 years ago.
        // Let's
        // try it again...
        menu.Append(XRCID("toggle_breakpoint_enabled_status"),
                    bp.is_enabled ? wxString(_("Disable Breakpoint")) : wxString(_("Enable Breakpoint")));
        menu.Append(XRCID("edit_breakpoint"), wxString(_("Edit Breakpoint")));
    }

    if (ManagerST::Get()->DbgCanInteract()) {
        menu.AppendSeparator();
        menu.Append(XRCID("dbg_run_to_cursor"), _("Run to here"));
    }

    clContextMenuEvent event(wxEVT_CONTEXT_MENU_EDITOR_MARGIN);
    event.SetMenu(&menu);
    if (EventNotifier::Get()->ProcessEvent(event))
        return;

    menu.Bind(
        wxEVT_MENU,
        [this, pt](wxCommandEvent& evt) {
            wxUnusedVar(evt);
            // build a command that can be used by gdb / lldb cli
            int line = LineFromPosition(PositionFromPoint(pt)) + 1;
            wxString set_breakpoint_cmd;
            set_breakpoint_cmd << "b " << GetRemotePathOrLocal() << ":" << line;
            ::CopyToClipboard(set_breakpoint_cmd);
            clGetManager()->SetStatusMessage(_("Breakpoint command copied to clipboard!"), 3);
        },
        XRCID("copy_breakpoint_format"));

    PopupMenu(&menu);
    m_popupIsOn = false;
}

void clEditor::AddOtherBreakpointType(wxCommandEvent& event)
{
    bool is_temp = (event.GetId() == XRCID("insert_temp_breakpoint"));
    bool is_disabled = (event.GetId() == XRCID("insert_disabled_breakpoint"));

    wxString conditions;
    if (event.GetId() == XRCID("insert_cond_breakpoint")) {
        conditions = wxGetTextFromUser(_("Enter the condition statement"), _("Create Conditional Breakpoint"));
        if (conditions.IsEmpty()) {
            return;
        }
    }

    AddBreakpoint(-1, conditions, is_temp, is_disabled);
}

void clEditor::OnIgnoreBreakpoint()
{
    if (ManagerST::Get()->GetBreakpointsMgr()->IgnoreByLineno(FileUtils::RealPath(GetFileName().GetFullPath()),
                                                              GetCurrentLine() + 1)) {
        clMainFrame::Get()->GetDebuggerPane()->GetBreakpointView()->Initialize();
    }
}

void clEditor::OnEditBreakpoint()
{
    ManagerST::Get()->GetBreakpointsMgr()->EditBreakpointByLineno(FileUtils::RealPath(GetFileName().GetFullPath()),
                                                                  GetCurrentLine() + 1);
    clMainFrame::Get()->GetDebuggerPane()->GetBreakpointView()->Initialize();
}

void clEditor::AddBreakpoint(int lineno /*= -1*/,
                             const wxString& conditions /*=wxT("")*/,
                             const bool is_temp /*=false*/,
                             const bool is_disabled /*=false*/)
{
    if (lineno == -1) {
        lineno = GetCurrentLine() + 1;
    }

    wxString file_path = GetRemotePathOrLocal();
    ManagerST::Get()->GetBreakpointsMgr()->SetExpectingControl(true);
    if (!ManagerST::Get()->GetBreakpointsMgr()->AddBreakpointByLineno(
            file_path, lineno, conditions, is_temp, is_disabled)) {
        wxMessageBox(_("Failed to insert breakpoint"));

    } else {

        clMainFrame::Get()->GetDebuggerPane()->GetBreakpointView()->Initialize();
        wxString message(_("Breakpoint successfully added")), prefix;
        if (is_temp) {
            prefix = _("Temporary ");
        } else if (is_disabled) {
            prefix = _("Disabled ");
        } else if (!conditions.IsEmpty()) {
            prefix = _("Conditional ");
        }
        m_mgr->GetStatusBar()->SetMessage(prefix + message);
    }
}

void clEditor::DelBreakpoint(int lineno /*= -1*/)
{
    if (lineno == -1) {
        lineno = GetCurrentLine() + 1;
    }
    wxString message;
    // enable the 'expectingControl' to 'true'
    // this is used by Manager class to detect whether the control
    // was triggered by user action
    ManagerST::Get()->GetBreakpointsMgr()->SetExpectingControl(true);

    wxString file_path = GetRemotePathOrLocal();
    int result = ManagerST::Get()->GetBreakpointsMgr()->DelBreakpointByLineno(file_path, lineno);
    switch (result) {
    case true:
        clMainFrame::Get()->GetDebuggerPane()->GetBreakpointView()->Initialize();
        m_mgr->GetStatusBar()->SetMessage(_("Breakpoint successfully deleted"));
        return;
    case wxID_CANCEL:
        return;
    case false:
        message = _("No breakpoint found on this line");
        break;
    default:
        message = _("Breakpoint deletion failed");
    }

    wxMessageBox(message, _("Breakpoint not deleted"), wxICON_ERROR | wxOK);
}

void clEditor::ToggleBreakpoint(int lineno)
{
    // Coming from OnMarginClick() means that lineno comes from the mouse position, not necessarily the current line
    if (lineno == -1) {
        lineno = GetCurrentLine() + 1;
    }

    wxString file_path = GetRemotePathOrLocal();

    // Does any of the plugins want to handle this?
    clDebugEvent dbgEvent(wxEVT_DBG_UI_TOGGLE_BREAKPOINT);
    dbgEvent.SetInt(lineno);
    dbgEvent.SetLineNumber(lineno);
    dbgEvent.SetFileName(file_path);
    if (clWorkspaceManager::Get().IsWorkspaceOpened()) {
        dbgEvent.SetDebuggerName(clWorkspaceManager::Get().GetWorkspace()->GetDebuggerName());

    } else {
        // use the global debugger selected in the quick debug view
        QuickDebugInfo info;
        EditorConfigST::Get()->ReadObject(wxT("QuickDebugDlg"), &info);

        wxArrayString debuggers = DebuggerMgr::Get().GetAvailableDebuggers();
        if (debuggers.empty() || info.GetSelectedDbg() < 0 || info.GetSelectedDbg() >= (int)debuggers.size()) {
            dbgEvent.SetDebuggerName(wxEmptyString);
        } else {
            dbgEvent.SetDebuggerName(debuggers[info.GetSelectedDbg()]);
        }
    }

    if (EventNotifier::Get()->ProcessEvent(dbgEvent)) {
        return;
    }

    const clDebuggerBreakpoint& bp = ManagerST::Get()->GetBreakpointsMgr()->GetBreakpoint(file_path, lineno);
    if (bp.IsNull()) {
        // This will (always?) be from a margin mouse-click, so assume it's a standard breakpt that's wanted
        AddBreakpoint(lineno);
    } else {
        DelBreakpoint(lineno);
    }
}

void clEditor::SetWarningMarker(int lineno, CompilerMessage&& msg)
{
    if (lineno < 0) {
        return;
    }

    // Keep the text message
    if (m_compilerMessagesMap.count(lineno)) {
        m_compilerMessagesMap.erase(lineno);
    }

    wxString display_message = msg.message;
    m_compilerMessagesMap.insert({ lineno, std::move(msg) });

    if (m_buildOptions.GetErrorWarningStyle() == BuildTabSettingsData::MARKER_BOOKMARKS) {
        MarkerAdd(lineno, smt_warning);
        NotifyMarkerChanged(lineno);
    }

    if (m_buildOptions.GetErrorWarningStyle() == BuildTabSettingsData::MARKER_ANNOTATE) {
        // define the warning marker
        AnnotationSetText(lineno, display_message);
        AnnotationSetStyle(lineno, ANNOTATION_STYLE_WARNING);
    }
}

void clEditor::SetErrorMarker(int lineno, CompilerMessage&& msg)
{
    if (lineno < 0) {
        return;
    }

    // Keep the text message
    if (m_compilerMessagesMap.count(lineno)) {
        m_compilerMessagesMap.erase(lineno);
    }

    wxString display_message = msg.message;
    m_compilerMessagesMap.insert({ lineno, std::move(msg) });

    if (m_buildOptions.GetErrorWarningStyle() == BuildTabSettingsData::MARKER_BOOKMARKS) {
        MarkerAdd(lineno, smt_error);
        NotifyMarkerChanged(lineno);
    }

    if (m_buildOptions.GetErrorWarningStyle() == BuildTabSettingsData::MARKER_ANNOTATE) {
        AnnotationSetText(lineno, display_message);
        AnnotationSetStyle(lineno, ANNOTATION_STYLE_ERROR);
    }
}

void clEditor::DelAllCompilerMarkers()
{
    MarkerDeleteAll(smt_warning);
    MarkerDeleteAll(smt_error);
    AnnotationClearAll();
    m_compilerMessagesMap.clear();

    // Notify about marker changes
    NotifyMarkerChanged();
}

// Maybe one day we'll display multiple bps differently
void clEditor::SetBreakpointMarker(int lineno,
                                   BreakpointType bptype,
                                   bool is_disabled,
                                   const std::vector<clDebuggerBreakpoint>& bps)
{
    BPtoMarker bpm = GetMarkerForBreakpt(bptype);
    sci_marker_types markertype = is_disabled ? bpm.marker_disabled : bpm.marker;
    int markerHandle = MarkerAdd(lineno - 1, markertype);
    NotifyMarkerChanged(lineno - 1);
    // keep the breakpoint info vector for this marker
    m_breakpointsInfo.insert(std::make_pair(markerHandle, bps));
}

void clEditor::DelAllBreakpointMarkers()
{
    // remove the stored information
    m_breakpointsInfo.clear();

    for (int bp_type = BP_FIRST_ITEM; bp_type <= BP_LAST_MARKED_ITEM; ++bp_type) {
        BPtoMarker bpm = GetMarkerForBreakpt((BreakpointType)bp_type);
        MarkerDeleteAll(bpm.marker);
        MarkerDeleteAll(bpm.marker_disabled);
    }
    // Notify about marker changes
    NotifyMarkerChanged();
}

void clEditor::HighlightLine(int lineno)
{
    if (GetLineCount() <= 0) {
        return;
    }

    if (GetLineCount() < lineno - 1) {
        lineno = GetLineCount() - 1;
    }
    MarkerAdd(lineno, smt_indicator);
    NotifyMarkerChanged(lineno);
}

void clEditor::UnHighlightAll()
{
    MarkerDeleteAll(smt_indicator); // Notify about marker changes
    NotifyMarkerChanged();
}

void clEditor::AddDebuggerContextMenu(wxMenu* menu)
{
    if (!ManagerST::Get()->DbgCanInteract()) {
        return;
    }

    wxString word = GetSelectedText();
    if (word.IsEmpty()) {
        word = GetWordAtCaret();
        if (word.IsEmpty()) {
            return;
        }
    }

    if (word.Contains("\n")) {
        // Don't create massive context menu
        return;
    }

    // Truncate the word
    if (word.length() > 20) {
        word = word.Mid(0, 20);
        word << "...";
    }

    m_customCmds.clear();
    wxString menuItemText;

    wxMenuItem* item;
    item = new wxMenuItem(menu, wxID_SEPARATOR);
    menu->Prepend(item);
    m_dynItems.push_back(item);

    //---------------------------------------------
    // Add custom commands
    //---------------------------------------------
    menu->Prepend(XRCID("debugger_watches"), _("More Watches"), DoCreateDebuggerWatchMenu(word));

    menuItemText.Clear();
    menuItemText << _("Add Watch") << wxT(" '") << word << wxT("'");
    item = new wxMenuItem(menu, wxNewId(), menuItemText);
    menu->Prepend(item);
    menu->Connect(
        item->GetId(), wxEVT_COMMAND_MENU_SELECTED, wxCommandEventHandler(clEditor::OnDbgAddWatch), NULL, this);
    m_dynItems.push_back(item);

    menuItemText.Clear();
    menu->Prepend(XRCID("dbg_run_to_cursor"), _("Run to Caret Line"), _("Run to Caret Line"));
    menu->Prepend(XRCID("dbg_jump_cursor"), _("Jump to Caret Line"), _("Jump to Caret Line"));
    m_dynItems.push_back(item);
}

void clEditor::OnDbgAddWatch(wxCommandEvent& event)
{
    wxUnusedVar(event);

    wxString word = GetSelectedText();
    if (word.IsEmpty()) {
        word = GetWordAtCaret();
        if (word.IsEmpty()) {
            return;
        }
    }
    clMainFrame::Get()->GetDebuggerPane()->GetWatchesTable()->AddExpression(word);
    clMainFrame::Get()->GetDebuggerPane()->SelectTab(wxGetTranslation(DebuggerPane::WATCHES));
    clMainFrame::Get()->GetDebuggerPane()->GetWatchesTable()->RefreshValues();
}

void clEditor::OnDbgCustomWatch(wxCommandEvent& event)
{
    wxUnusedVar(event);
    wxString word = GetSelectedText();
    if (word.IsEmpty()) {
        word = GetWordAtCaret();
        if (word.IsEmpty()) {
            return;
        }
    }

    // find the custom command to run
    std::map<int, wxString>::iterator iter = m_customCmds.find(event.GetId());
    if (iter != m_customCmds.end()) {

        // Replace $(Variable) with the actual string
        wxString command = iter->second;
        command = MacroManager::Instance()->Replace(command, wxT("variable"), word, true);

        clMainFrame::Get()->GetDebuggerPane()->GetWatchesTable()->AddExpression(command);
        clMainFrame::Get()->GetDebuggerPane()->SelectTab(wxGetTranslation(DebuggerPane::WATCHES));
        clMainFrame::Get()->GetDebuggerPane()->GetWatchesTable()->RefreshValues();
    }
}

void clEditor::UpdateColours() { Colourise(0, wxSTC_INVALID_POSITION); }

int clEditor::SafeGetChar(int pos)
{
    if (pos < 0 || pos >= GetLength()) {
        return 0;
    }
    return GetCharAt(pos);
}

void clEditor::OnDragStart(wxStyledTextEvent& e)
{
    m_isDragging = true; // Otherwise it sometimes obscures the desired drop zone!
    e.Skip();
}

void clEditor::OnDragEnd(wxStyledTextEvent& e)
{
    // For future reference, this will only be called when D'n'D ends successfully with a drop.
    // Unfortunately scintilla doesn't seem to provide any notification when ESC is pressed, or the drop-zone is
    // invalid
    m_isDragging = false; // Turn on calltips again

    e.Skip();
}

int clEditor::GetCurrLineHeight()
{
    int point = GetCurrentPos();
    wxPoint pt = PointFromPosition(point);

    // calculate the line height
    int curline = LineFromPosition(point);
    int ll;
    int hh(0);
    if (curline > 0) {
        ll = curline - 1;
        int pp = PositionFromLine(ll);
        wxPoint p = PointFromPosition(pp);
        hh = pt.y - p.y;
    } else {
        ll = curline + 1;
        int pp = PositionFromLine(ll);
        wxPoint p = PointFromPosition(pp);
        hh = p.y - pt.y;
    }

    if (hh == 0) {
        hh = 12; // default height on most OSs
    }

    return hh;
}

void clEditor::DoHighlightWord()
{
    // Read the primary selected text
    int mainSelectionStart = GetSelectionNStart(GetMainSelection());
    int mainSelectionEnd = GetSelectionNEnd(GetMainSelection());
    wxString word = GetTextRange(mainSelectionStart, mainSelectionEnd);

    wxString selectedTextTrimmed = word;
    selectedTextTrimmed.Trim().Trim(false);
    if (selectedTextTrimmed.IsEmpty()) {
        return;
    }

    // Search only the visible areas
    StringHighlighterJob j;
    int firstVisibleLine = GetFirstVisibleLine();
    int lastDocLine = LineFromPosition(GetLength());
    int offset = PositionFromLine(firstVisibleLine);

    if (GetAllLinesVisible()) {
        // The simple case: there aren't any folds
        int lastLine = firstVisibleLine + LinesOnScreen();
        if (lastLine > lastDocLine) {
            lastLine = lastDocLine;
        }
        int lastPos = PositionFromLine(lastLine) + LineLength(lastLine);
        wxString text = GetTextRange(offset, lastPos);
        j.Set(text, word, offset);
        j.Process();
    } else {

        // There are folds, so we have to process each visible section separately
        firstVisibleLine = DocLineFromVisible(firstVisibleLine); // This copes with folds above the displayed lines
        int lineCount(0);
        int nextLineToProcess(firstVisibleLine);
        int screenLines(LinesOnScreen());
        while (lineCount < screenLines && nextLineToProcess <= lastDocLine) {
            int offset(-1);
            int line = nextLineToProcess;

            // Skip over any invisible lines
            while (!GetLineVisible(line) && line < lastDocLine) {
                ++line;
            }

            // EOF?
            if (line >= lastDocLine)
                break;

            while (GetLineVisible(line) && line <= lastDocLine) {
                if (offset == -1) {
                    offset = PositionFromLine(line); // Get offset value the first time through
                }
                ++line;
                ++lineCount;
                if (lineCount >= screenLines) {
                    break;
                }
            }
            if (line > lastDocLine) {
                line = lastDocLine;
            }
            nextLineToProcess = line;

            int lastPos = PositionFromLine(nextLineToProcess) + LineLength(nextLineToProcess);
            wxString text = GetTextRange(offset, lastPos);
            j.Set(text, word, offset);
            j.Process();
        }
    }

    // Keep the first offset
    m_highlightedWordInfo.Clear();
    m_highlightedWordInfo.SetFirstOffset(offset);
    m_highlightedWordInfo.SetWord(word);
    HighlightWord((StringHighlightOutput*)&j.GetOutput());
}

void clEditor::HighlightWord(bool highlight)
{
    if (highlight) {
        DoHighlightWord();

    } else if (m_highlightedWordInfo.IsHasMarkers()) {
        SetIndicatorCurrent(INDICATOR_WORD_HIGHLIGHT);
        IndicatorClearRange(0, GetLength());
        m_highlightedWordInfo.Clear();
    }
}

void clEditor::OnLeftDClick(wxStyledTextEvent& event)
{
    long highlight_word = EditorConfigST::Get()->GetInteger(wxT("highlight_word"), 0);
    if (GetSelectedText().IsEmpty() == false && highlight_word) {
        DoHighlightWord();
    }
    event.Skip();
}

bool clEditor::IsCompletionBoxShown() { return wxCodeCompletionBoxManager::Get().IsShown(); }

int clEditor::GetCurrentLine()
{
    // return the current line number
    int pos = GetCurrentPos();
    return LineFromPosition(pos);
}

void clEditor::DoSetCaretAt(wxStyledTextCtrl* ctrl, long pos)
{
    ctrl->SetCurrentPos(pos);
    ctrl->SetSelectionStart(pos);
    ctrl->SetSelectionEnd(pos);
    int line = ctrl->LineFromPosition(pos);
    if (line >= 0) {
        // This is needed to unfold the line if it were folded
        // The various other 'EnsureVisible' things don't do this
        ctrl->EnsureVisible(line);
    }
}

int clEditor::GetEOLByContent()
{
    if (GetLength() == 0) {
        return wxNOT_FOUND;
    }

    // locate the first EOL
    wxString txt = GetText();
    size_t pos1 = static_cast<size_t>(txt.Find(wxT("\n")));
    size_t pos2 = static_cast<size_t>(txt.Find(wxT("\r\n")));
    size_t pos3 = static_cast<size_t>(txt.Find(wxT("\r")));

    size_t max_size_t = static_cast<size_t>(-1);
    // the buffer is not empty but it does not contain any EOL as well
    if (pos1 == max_size_t && pos2 == max_size_t && pos3 == max_size_t) {
        return wxNOT_FOUND;
    }

    size_t first_eol_pos(0);
    pos2 < pos1 ? first_eol_pos = pos2 : first_eol_pos = pos1;
    if (pos3 < first_eol_pos) {
        first_eol_pos = pos3;
    }

    // get the EOL at first_eol_pos
    wxChar ch = SafeGetChar(first_eol_pos);
    if (ch == wxT('\n')) {
        return wxSTC_EOL_LF;
    }

    if (ch == wxT('\r')) {
        wxChar secondCh = SafeGetChar(first_eol_pos + 1);
        if (secondCh == wxT('\n')) {
            return wxSTC_EOL_CRLF;
        } else {
            return wxSTC_EOL_CR;
        }
    }
    return wxNOT_FOUND;
}

int clEditor::GetEOLByOS()
{
    OptionsConfigPtr options = GetOptions();
    if (options->GetEolMode() == wxT("Unix (LF)")) {
        return wxSTC_EOL_LF;
    } else if (options->GetEolMode() == wxT("Mac (CR)")) {
        return wxSTC_EOL_CR;
    } else if (options->GetEolMode() == wxT("Windows (CRLF)")) {
        return wxSTC_EOL_CRLF;
    } else {
// set the EOL by the hosting OS
#if defined(__WXMAC__)
        return wxSTC_EOL_LF;
#elif defined(__WXGTK__)
        return wxSTC_EOL_LF;
#else
        return wxSTC_EOL_CRLF;
#endif
    }
}

void clEditor::ShowFunctionTipFromCurrentPos()
{
    if (TagsManagerST::Get()->GetCtagsOptions().GetFlags() & CC_DISP_FUNC_CALLTIP) {
        int pos = DoGetOpenBracePos();
        // see if any of the plugins want to handle it
        clCodeCompletionEvent evt(wxEVT_CC_CODE_COMPLETE_FUNCTION_CALLTIP, GetId());
        evt.SetPosition(pos);
        evt.SetInsideCommentOrString(m_context->IsCommentOrString(pos));
        evt.SetFileName(FileUtils::RealPath(GetFileName().GetFullPath()));
        EventNotifier::Get()->ProcessEvent(evt);
    }
}

wxString clEditor::GetSelection() { return wxStyledTextCtrl::GetSelectedText(); }

int clEditor::GetSelectionStart() { return wxStyledTextCtrl::GetSelectionStart(); }

int clEditor::GetSelectionEnd() { return wxStyledTextCtrl::GetSelectionEnd(); }

void clEditor::ReplaceSelection(const wxString& text) { wxStyledTextCtrl::ReplaceSelection(text); }

void clEditor::ClearUserIndicators()
{
    SetIndicatorCurrent(INDICATOR_USER);
    IndicatorClearRange(0, GetLength());
}

int clEditor::GetUserIndicatorEnd(int pos) { return wxStyledTextCtrl::IndicatorEnd(INDICATOR_USER, pos); }

int clEditor::GetUserIndicatorStart(int pos) { return wxStyledTextCtrl::IndicatorStart(INDICATOR_USER, pos); }

void clEditor::SelectText(int startPos, int len)
{
    SetSelectionStart(startPos);
    SetSelectionEnd(startPos + len);
}

void clEditor::SetUserIndicator(int startPos, int len)
{
    SetIndicatorCurrent(INDICATOR_USER);
    IndicatorFillRange(startPos, len);
}

void clEditor::SetUserIndicatorStyleAndColour(int style, const wxColour& colour)
{
    IndicatorSetForeground(INDICATOR_USER, colour);
    IndicatorSetStyle(INDICATOR_USER, style);
    IndicatorSetUnder(INDICATOR_USER, false);
    IndicatorSetAlpha(INDICATOR_USER, wxSTC_ALPHA_NOALPHA);
}

int clEditor::GetLexerId() { return GetLexer(); }

int clEditor::GetStyleAtPos(int pos) { return GetStyleAt(pos); }

int clEditor::WordStartPos(int pos, bool onlyWordCharacters)
{
    return wxStyledTextCtrl::WordStartPosition(pos, onlyWordCharacters);
}

int clEditor::WordEndPos(int pos, bool onlyWordCharacters)
{
    return wxStyledTextCtrl::WordEndPosition(pos, onlyWordCharacters);
}

void clEditor::DoMarkHyperlink(wxMouseEvent& event, bool isMiddle)
{
    if (event.m_controlDown || isMiddle) {
        SetIndicatorCurrent(INDICATOR_HYPERLINK);
        long pos = PositionFromPointClose(event.GetX(), event.GetY());

        wxColour bgCol = StyleGetBackground(0);
        if (DrawingUtils::IsDark(bgCol)) {
            IndicatorSetForeground(INDICATOR_HYPERLINK, *wxWHITE);
        } else {
            IndicatorSetForeground(INDICATOR_HYPERLINK, *wxBLUE);
        }

        if (pos != wxSTC_INVALID_POSITION) {
            if (m_context->GetHyperlinkRange(m_hyperLinkIndicatroStart, m_hyperLinkIndicatroEnd)) {
                IndicatorFillRange(m_hyperLinkIndicatroStart, m_hyperLinkIndicatroEnd - m_hyperLinkIndicatroStart);
                SetSTCCursor(8);
            } else {
                m_hyperLinkIndicatroStart = wxNOT_FOUND;
                m_hyperLinkIndicatroEnd = wxNOT_FOUND;
            }
        }
    }
}

void clEditor::DoQuickJump(wxMouseEvent& event, bool isMiddle)
{
    wxUnusedVar(isMiddle);
    if (m_hyperLinkIndicatroStart == wxNOT_FOUND || m_hyperLinkIndicatroEnd == wxNOT_FOUND)
        return;

    // indicator is highlighted
    long pos = PositionFromPointClose(event.GetX(), event.GetY());
    if (m_hyperLinkIndicatroStart <= pos && pos <= m_hyperLinkIndicatroEnd) {
        // bool altLink = (isMiddle && event.m_controlDown) || (!isMiddle && event.m_altDown);

        // Let the plugins handle it first
        clCodeCompletionEvent jump_event(wxEVT_CC_JUMP_HYPER_LINK);
        jump_event.SetFileName(FileUtils::RealPath(GetFileName().GetFullPath()));
        EventNotifier::Get()->ProcessEvent(jump_event);
    }

    // clear the hyper link indicators
    m_hyperLinkIndicatroStart = wxNOT_FOUND;
    m_hyperLinkIndicatroEnd = wxNOT_FOUND;

    SetIndicatorCurrent(INDICATOR_HYPERLINK);
    IndicatorClearRange(0, GetLength());
    event.Skip();
}

void clEditor::TrimText(size_t flags)
{
    bool trim = flags & TRIM_ENABLED;
    bool appendLf = flags & TRIM_APPEND_LF;
    bool dontTrimCaretLine = flags & TRIM_IGNORE_CARET_LINE;
    bool trimOnlyModifiedLInes = flags & TRIM_MODIFIED_LINES;

    if (!trim && !appendLf) {
        return;
    }

    // wrap the entire operation in a single undo action
    BeginUndoAction();

    if (trim) {
        int maxLines = GetLineCount();
        int currLine = GetCurrentLine();
        for (int line = 0; line < maxLines; line++) {

            // only trim lines modified by the user in this session
            bool is_modified_line = ((size_t)line < m_modifiedLines.size()) && (m_modifiedLines[line] == LINE_MODIFIED);
            if (trimOnlyModifiedLInes && !is_modified_line) {
                continue;
            }

            // We can trim in the following cases:
            // 1) line is NOT the caret line OR
            // 2) line is the caret line, however dontTrimCaretLine is FALSE
            bool canTrim = ((line != currLine) || (line == currLine && !dontTrimCaretLine));
            if (!canTrim) {
                continue;
            }

            int lineStart = PositionFromLine(line);
            int lineEnd = GetLineEndPosition(line);
            int i = lineEnd - 1;
            wxChar ch = (wxChar)(GetCharAt(i));
            while ((i >= lineStart) && ((ch == _T(' ')) || (ch == _T('\t')))) {
                i--;
                ch = (wxChar)(GetCharAt(i));
            }
            if (i < (lineEnd - 1)) {
                SetTargetStart(i + 1);
                SetTargetEnd(lineEnd);
                ReplaceTarget(_T(""));
            }
        }
    }

    if (appendLf) {
        // The following code was adapted from the SciTE sourcecode
        int maxLines = GetLineCount();
        int enddoc = PositionFromLine(maxLines);
        if (maxLines <= 1 || enddoc > PositionFromLine(maxLines - 1))
            InsertText(enddoc, GetEolString());
    }

    EndUndoAction();
}

void clEditor::TrimText(bool trim, bool appendLf)
{
    size_t flags = 0;
    if (trim) {
        flags |= TRIM_ENABLED;
    }
    if (appendLf) {
        flags |= TRIM_APPEND_LF;
    }
    if (GetOptions()->GetTrimOnlyModifiedLines()) {
        flags |= TRIM_MODIFIED_LINES;
    }
    if (GetOptions()->GetDontTrimCaretLine()) {
        flags |= TRIM_IGNORE_CARET_LINE;
    }
    TrimText(flags);
}

wxString clEditor::GetEolString()
{
    wxString eol;
    switch (this->GetEOLMode()) {
    case wxSTC_EOL_CR:
        eol = wxT("\r");
        break;
    case wxSTC_EOL_CRLF:
        eol = wxT("\r\n");
        break;
    case wxSTC_EOL_LF:
        eol = wxT("\n");
        break;
    }
    return eol;
}

void clEditor::DoShowCalltip(int pos, const wxString& title, const wxString& tip, bool strip_html_tags)
{
    DoCancelCalltip();
    wxPoint pt;
    wxString tooltip;
    tooltip << title;
    tooltip.Trim().Trim(false);
    if (!tooltip.empty()) {
        tooltip << "\n---\n";
    }
    tooltip << tip;
    m_calltip = new CCBoxTipWindow(this, tooltip, strip_html_tags);
    if (pos == wxNOT_FOUND) {
        pt = ::wxGetMousePosition();
    } else {
        pt = PointFromPosition(pos);
    }

    DoAdjustCalltipPos(pt);
    m_calltip->CallAfter(&CCBoxTipWindow::PositionAt, pt, this);
}

void clEditor::DoAdjustCalltipPos(wxPoint& pt) const
{
    wxSize size = m_calltip->GetSize();
    int disp = wxDisplay::GetFromPoint(pt);
    wxRect rect = wxDisplay(disp == wxNOT_FOUND ? 0 : disp).GetClientArea();
    auto checkX = [&](int xx) { return xx >= rect.GetX() && xx <= rect.GetX() + rect.GetWidth(); };
    auto checkY = [&](int yy) { return yy >= rect.GetY() && yy <= rect.GetY() + rect.GetHeight(); };
    // if neither fits, put at the rightmost/topmost of the display screen
    int x = rect.GetX() + rect.GetWidth() - size.GetWidth();
    int y = rect.GetY();
    if (checkX(pt.x + size.GetWidth())) {
        // right of the mouse position (preferred)
        x = pt.x;
    } else if (checkX(pt.x - size.GetWidth())) {
        // left of the mouse position
        x = pt.x - size.GetWidth();
    }
    if (checkY(pt.y - size.GetHeight())) {
        // top of the mouse position (preferred)
        y = pt.y - size.GetHeight();
    } else if (checkY(pt.y + size.GetHeight())) {
        // bottom of the mouse position
        y = pt.y;
    }
    pt = { x, y };
}

void clEditor::DoCancelCalltip()
{
    CallTipCancel();
    DoCancelCodeCompletionBox();
}

int clEditor::DoGetOpenBracePos()
{
    if (m_calltip && m_calltip->IsShown()) {
        return m_calltip->GetEditorStartPosition();
    }

    // determine the closest open brace from the current caret position
    int depth(0);
    int char_tested(0); // we add another performance tuning here: dont test more than 256 characters backward
    bool exit_loop(false);

    int pos = PositionBefore(GetCurrentPos());
    while ((pos > 0) && (char_tested < 256)) {
        wxChar ch = SafeGetChar(pos);
        if (m_context->IsCommentOrString(pos)) {
            pos = PositionBefore(pos);
            continue;
        }

        char_tested++;

        switch (ch) {
        case wxT('{'):
            depth++;
            pos = PositionBefore(pos);
            break;
        case wxT('}'):
            depth--;
            pos = PositionBefore(pos);
            break;
        case wxT(';'):
            exit_loop = true;
            break;
        case wxT('('):
            depth++;
            if (depth == 1) {
                pos = PositionAfter(pos);
                exit_loop = true;
            } else {
                pos = PositionBefore(pos);
            }
            break;
        case wxT(')'):
            depth--;
        // fall through
        default:
            pos = PositionBefore(pos);
            break;
        }

        if (exit_loop)
            break;
    }

    if (char_tested == 256) {
        return wxNOT_FOUND;
    } else if (depth == 1 && pos >= 0) {
        return pos;
    }
    return wxNOT_FOUND;
}

void clEditor::SetEOL()
{
    // set the EOL mode
    int eol = GetEOLByOS();
    int alternate_eol = GetEOLByContent();
    if (alternate_eol != wxNOT_FOUND) {
        eol = alternate_eol;
    }
    SetEOLMode(eol);
}

void clEditor::OnChange(wxStyledTextEvent& event)
{
    event.Skip();
    ++m_modificationCount;

    int modification_flags = event.GetModificationType();
    bool isCoalesceStart = modification_flags & wxSTC_STARTACTION;
    bool isInsert = modification_flags & wxSTC_MOD_INSERTTEXT;
    bool isDelete = modification_flags & wxSTC_MOD_DELETETEXT;
    bool isUndo = modification_flags & wxSTC_PERFORMED_UNDO;
    bool isRedo = modification_flags & wxSTC_PERFORMED_REDO;

    bool line_numbers_margin_updated = false;
    if (isUndo || isRedo) {
        // update line numbers on the next event loop
        NotifyTextUpdated();
        line_numbers_margin_updated = true;
    }

    if (!line_numbers_margin_updated) {
        int newLineCount = GetLineCount();
        if (m_lastLineCount != newLineCount) {
            int lastWidthCount = log10(m_editorState.current_line) + 1;
            int newWidthCount = log10(newLineCount) + 1;
            if (newWidthCount != lastWidthCount) {
                NotifyTextUpdated();
                line_numbers_margin_updated = true;
            }
        }
    }

    // Notify about this editor being changed
    if (GetModify()) {
        clCommandEvent eventMod(wxEVT_EDITOR_MODIFIED);
        eventMod.SetFileName(FileUtils::RealPath(GetFileName().GetFullPath()));
        EventNotifier::Get()->QueueEvent(eventMod.Clone());
    }

    if ((m_autoAddNormalBraces && !m_disableSmartIndent) || GetOptions()->GetAutoCompleteDoubleQuotes()) {
        if ((event.GetModificationType() & wxSTC_MOD_BEFOREDELETE) &&
            (event.GetModificationType() & wxSTC_PERFORMED_USER)) {
            wxString deletedText = GetTextRange(event.GetPosition(), event.GetPosition() + event.GetLength());
            if (deletedText.IsEmpty() == false && deletedText.Length() == 1) {
                if (deletedText.GetChar(0) == wxT('[') || deletedText.GetChar(0) == wxT('(')) {
                    int where = wxStyledTextCtrl::BraceMatch(event.GetPosition());
                    if (where != wxNOT_FOUND) {
                        wxCommandEvent e(wxCMD_EVENT_REMOVE_MATCH_INDICATOR);
                        // the removal will take place after the actual deletion of the
                        // character, so we set it to be position before
                        e.SetInt(PositionBefore(where));
                        AddPendingEvent(e);
                    }
                } else if (deletedText.GetChar(0) == '\'' || deletedText.GetChar(0) == '"') {

                    wxChar searchChar = deletedText.GetChar(0);
                    // search for the matching close quote
                    int from = event.GetPosition() + 1;
                    int until = GetLineEndPosition(GetCurrentLine());

                    for (int i = from; i < until; ++i) {
                        if (SafeGetChar(i) == searchChar) {
                            wxCommandEvent e(wxCMD_EVENT_REMOVE_MATCH_INDICATOR);
                            // the removal will take place after the actual deletion of the
                            // character, so we set it to be position before
                            e.SetInt(PositionBefore(i));
                            AddPendingEvent(e);
                        }
                    }
                }
            }
        }
    }

    if (isCoalesceStart && GetCommandsProcessor().HasOpenCommand()) {
        // The user has changed mode e.g. from inserting to deleting, so the current command must be closed
        GetCommandsProcessor().CommandProcessorBase::ProcessOpenCommand(); // Use the base-class method, as this time we
                                                                           // don't need to tell scintilla too
    }

    if (isInsert || isDelete) {

        if (!GetReloadingFile() && !isUndo && !isRedo) {
            CLCommand::Ptr_t currentOpen = GetCommandsProcessor().GetOpenCommand();
            if (!currentOpen) {
                GetCommandsProcessor().StartNewTextCommand(isInsert ? CLC_insert : CLC_delete);
            }
            // We need to cope with a selection being deleted by typing; this results in 0x2012 followed immediately
            // by
            // 0x11 i.e. with no intervening wxSTC_STARTACTION
            else if (isInsert && currentOpen->GetCommandType() != CLC_insert) {
                GetCommandsProcessor().ProcessOpenCommand();
                GetCommandsProcessor().StartNewTextCommand(CLC_insert);

            } else if (isDelete && currentOpen->GetCommandType() != CLC_delete) {
                GetCommandsProcessor().ProcessOpenCommand();
                GetCommandsProcessor().StartNewTextCommand(CLC_delete);
            }

            wxCHECK_RET(GetCommandsProcessor().HasOpenCommand(), "Trying to add to a non-existent or closed command");
            wxCHECK_RET(GetCommandsProcessor().CanAppend(isInsert ? CLC_insert : CLC_delete),
                        "Trying to add to the wrong type of command");
            GetCommandsProcessor().AppendToTextCommand(event.GetText(), event.GetPosition());
        }

        // Cache details of the number of lines added/removed
        // This is used to 'update' any affected FindInFiles result. See bug 3153847
        if (event.GetModificationType() & wxSTC_PERFORMED_UNDO) {
            m_deltas->Pop();
        } else {
            m_deltas->Push(event.GetPosition(),
                           event.GetLength() * (event.GetModificationType() & wxSTC_MOD_DELETETEXT ? -1 : 1));
        }

        int numlines(event.GetLinesAdded());

        if (numlines) {
            if (GetReloadingFile() == false) {
                // a line was added to or removed from the document, so synchronize the breakpoints on this editor
                // and the breakpoint manager
                UpdateBreakpoints();
            } else {
                // The file has been reloaded, so the cached line-changes are no longer relevant
                m_deltas->Clear();
            }
        }

        // ignore this event incase we are in the middle of file reloading
        if (!GetReloadingFile()) {
            // keep track of modified lines
            int curline = LineFromPosition(event.GetPosition());
            if (numlines == 0) {
                // probably only the current line was modified
                m_modifiedLines[curline] = LINE_MODIFIED;
            } else {
                for (int i = 0; i <= numlines; i++) {
                    m_modifiedLines[curline + i] = LINE_MODIFIED;
                }
            }
        }
    }
}

void clEditor::OnRemoveMatchInidicator(wxCommandEvent& e)
{
    // get the current indicator end range
    if (IndicatorValueAt(INDICATOR_MATCH, e.GetInt()) == 1) {
        int curpos = GetCurrentPos();
        SetSelection(e.GetInt(), e.GetInt() + 1);
        ReplaceSelection(wxEmptyString);
        SetCaretAt(curpos);
    }
}

bool clEditor::FindAndSelect(const wxString& pattern, const wxString& what, int pos, NavMgr* navmgr)
{
    return DoFindAndSelect(pattern, what, pos, navmgr);
}

void clEditor::DoSelectRange(const LSP::Range& range, bool center_line)
{
    ClearSelections();
    auto getPos = [this](const LSP::Position& param) -> int {
        int linePos = PositionFromLine(param.GetLine());
#if wxCHECK_VERSION(3, 1, 0)
        return PositionRelative(linePos, param.GetCharacter());
#else
        wxString text = GetLine(param.GetLine()).Truncate(param.GetCharacter());
        return linePos + clUTF8Length(text.wc_str(), text.length());
#endif
    };
    SetSelectionStart(getPos(range.GetStart()));
    SetSelectionEnd(getPos(range.GetEnd()));

    if (center_line) {
        int lineNumber = range.GetStart().GetLine();
        CallAfter(&clEditor::CenterLinePreserveSelection, lineNumber);
    }
}

bool clEditor::SelectLocation(const LSP::Location& location)
{
    int lineNumber = location.GetRange().GetStart().GetLine();
    int pos = PositionFromLine(lineNumber);
    return DoFindAndSelect(location.GetName(), location.GetName(), pos, nullptr);
}

bool clEditor::SelectRangeAfter(const LSP::Range& range)
{
    // on GTK, DoSelectRange will probably fail since the file is not really loaded into screen yet
    // so we need to use here CallAfter
    CallAfter(&clEditor::DoSelectRange, range, true);
    return true;
}

void clEditor::SelectRange(const LSP::Range& range) { DoSelectRange(range, false); }

bool clEditor::DoFindAndSelect(const wxString& _pattern, const wxString& what, int start_pos, NavMgr* navmgr)
{
    BrowseRecord jumpfrom = CreateBrowseRecord();

    bool realPattern(false);
    wxString pattern(_pattern);
    pattern.StartsWith(wxT("/^"), &pattern);
    if (_pattern.Length() != pattern.Length()) {
        realPattern = true;
    }

    if (pattern.EndsWith(wxT("$/"))) {
        pattern = pattern.Left(pattern.Len() - 2);
        realPattern = true;
    } else if (pattern.EndsWith(wxT("/"))) {
        pattern = pattern.Left(pattern.Len() - 1);
        realPattern = true;
    }

    size_t flags = wxSD_MATCHCASE | wxSD_MATCHWHOLEWORD;

    pattern.Trim();
    if (pattern.IsEmpty())
        return false;

    // keep current position
    long curr_pos = GetCurrentPos();
    int match_len(0), pos(0);

    // set the caret at the document start
    if (start_pos < 0 || start_pos > GetLength()) {
        start_pos = 0;
    }

    // set the starting point
    SetCurrentPos(0);
    SetSelectionStart(0);
    SetSelectionEnd(0);

    int offset(start_pos);
    bool again(false);
    bool res(false);

    do {
        again = false;
        flags = wxSD_MATCHCASE | wxSD_MATCHWHOLEWORD;

        if (StringFindReplacer::Search(GetText().wc_str(), offset, pattern.wc_str(), flags, pos, match_len)) {

            int line = LineFromPosition(pos);
            wxString dbg_line = GetLine(line).Trim().Trim(false);

            wxString tmp_pattern(pattern);
            tmp_pattern.Trim().Trim(false);
            if (dbg_line.Len() != tmp_pattern.Len() && tmp_pattern != what) {
                offset = pos + match_len;
                again = true;
            } else {

                // select only the name at the given text range
                wxString display_name = what.BeforeFirst(wxT('('));

                int match_len1(0), pos1(0);
                flags |= wxSD_SEARCH_BACKWARD;
                flags |= wxSD_MATCHWHOLEWORD;

                if (realPattern) {
                    // the inner search is done on the pattern without the part of the
                    // signature
                    pattern = pattern.BeforeFirst(wxT('('));
                }

                if (StringFindReplacer::Search(pattern.wc_str(),
                                               clUTF8Length(pattern.wc_str(), pattern.Len()),
                                               display_name.wc_str(),
                                               flags,
                                               pos1,
                                               match_len1)) {

                    // select only the word
                    // Check that pos1 is *not* 0 otherwise will get into an infinite loop
                    if (pos1 && GetContext()->IsCommentOrString(pos + pos1)) {
                        // try again
                        offset = pos + pos1;
                        again = true;
                    } else {
                        SetSelection(pos + pos1, pos + pos1 + match_len1);
                        res = true;
                    }
                } else {

                    // as a fallback, mark the whole line
                    ClearSelections();
                    SetCurrentPos(pos);
                    SetSelectionStart(pos);
                    SetSelectionEnd(pos + match_len);
                    res = true;
                }

                if (res && (line >= 0) && !again) {
                    SetEnsureCaretIsVisible(pos);
                    SetLineVisible(LineFromPosition(pos));
                    CenterLinePreserveSelection(LineFromPosition(pos));
                }
            }

        } else {
            // match failed, restore the caret
            SetCurrentPos(curr_pos);
            SetSelectionStart(curr_pos);
            SetSelectionEnd(curr_pos);
        }
    } while (again);

    if (res && navmgr) {
        auto new_loc = CreateBrowseRecord();
        if (!new_loc.IsSameAs(jumpfrom)) {
            navmgr->StoreCurrentLocation(jumpfrom, new_loc);
        }
    }
    this->ScrollToColumn(0);
    return res;
}

wxMenu* clEditor::DoCreateDebuggerWatchMenu(const wxString& word)
{
    DebuggerSettingsPreDefMap data;
    DebuggerConfigTool::Get()->ReadObject(wxT("DebuggerCommands"), &data);

    DebuggerPreDefinedTypes preDefTypes = data.GetActiveSet();
    DebuggerCmdDataVec cmds = preDefTypes.GetCmds();

    wxMenu* menu = new wxMenu();
    wxMenuItem* item(NULL);
    wxString menuItemText;

    for (size_t i = 0; i < cmds.size(); i++) {
        DebuggerCmdData cmd = cmds.at(i);
        menuItemText.Clear();
        menuItemText << _("Watch") << wxT(" '") << word << wxT("' ") << _("as") << wxT(" '") << cmd.GetName()
                     << wxT("'");
        item = new wxMenuItem(menu, wxNewId(), menuItemText);
        menu->Prepend(item);
        Connect(
            item->GetId(), wxEVT_COMMAND_MENU_SELECTED, wxCommandEventHandler(clEditor::OnDbgCustomWatch), NULL, this);
        m_customCmds[item->GetId()] = cmd.GetCommand();
    }

    return menu;
}

void clEditor::UpdateOptions()
{
    // Start by getting the global settings
    m_options = EditorConfigST::Get()->GetOptions();

    // Now let any local preferences overwrite the global equivalent
    if (clCxxWorkspaceST::Get()->IsOpen()) {
        clCxxWorkspaceST::Get()->GetLocalWorkspace()->GetOptions(m_options, GetProject());
    }

    EditorConfigST::Get()->ReadObject(wxT("BuildTabSettings"), &m_buildOptions);

    clEditorConfigEvent event(wxEVT_EDITOR_CONFIG_LOADING);
    event.SetFileName(FileUtils::RealPath(GetFileName().GetFullPath()));
    if (EventNotifier::Get()->ProcessEvent(event)) {
        m_options->UpdateFromEditorConfig(event.GetEditorConfig());
    }
}

bool clEditor::ReplaceAllExactMatch(const wxString& what, const wxString& replaceWith)
{
    int offset(0);
    wxString findWhat = what;
    size_t flags = wxSD_MATCHWHOLEWORD | wxSD_MATCHCASE;

    int pos(0);
    int match_len(0);
    int posInChars(0);
    int match_lenInChars(0);
    int matchCount(0);
    wxString txt = GetText();

    while (StringFindReplacer::Search(
        txt.wc_str(), offset, findWhat.wc_str(), flags, pos, match_len, posInChars, match_lenInChars)) {
        txt.Remove(posInChars, match_lenInChars);
        txt.insert(posInChars, replaceWith);
        matchCount++;
        offset = pos + clUTF8Length(replaceWith.wc_str(), replaceWith.length()); // match_len;
    }

    // replace the buffer
    BeginUndoAction();
    long savedPos = GetCurrentPos();

    SetText(txt);
    // Restore the caret
    SetCaretAt(savedPos);

    EndUndoAction();
    return (matchCount > 0);
}

void clEditor::SetLexerName(const wxString& lexerName) { SetSyntaxHighlight(lexerName); }

void clEditor::HighlightWord(StringHighlightOutput* highlightOutput)
{
    // the search highlighter thread has completed the calculations, fetch the results and mark them in the editor
    const std::vector<std::pair<int, int>>& matches = highlightOutput->matches;
    SetIndicatorCurrent(INDICATOR_WORD_HIGHLIGHT);

    // clear the old markers
    IndicatorClearRange(0, GetLength());
    if (!highlightOutput->matches.empty()) {
        m_highlightedWordInfo.SetHasMarkers(true);
        int selStart = GetSelectionStart();
        for (size_t i = 0; i < matches.size(); i++) {
            const std::pair<int, int>& p = matches.at(i);

            // Dont highlight the current selection
            if (p.first != selStart) {
                IndicatorFillRange(p.first, p.second);
            }
        }
    } else {
        m_highlightedWordInfo.Clear();
    }
}

void clEditor::ChangeCase(bool toLower)
{
    bool hasSelection = (GetSelectedText().IsEmpty() == false);

    if (hasSelection) {

        // Simply change the case of the selection
        toLower ? LowerCase() : UpperCase();

    } else {

        if (GetCurrentPos() >= GetLength())
            return;

        // Select the char
        SelectText(GetCurrentPos(), 1);
        toLower ? LowerCase() : UpperCase();
        CharRight();
    }
}

int clEditor::LineFromPos(int pos) { return wxStyledTextCtrl::LineFromPosition(pos); }

int clEditor::PosFromLine(int line) { return wxStyledTextCtrl::PositionFromLine(line); }

int clEditor::LineEnd(int line)
{
    int pos = wxStyledTextCtrl::PositionFromLine(line);
    return pos + wxStyledTextCtrl::LineLength(line);
}

wxString clEditor::GetTextRange(int startPos, int endPos) { return wxStyledTextCtrl::GetTextRange(startPos, endPos); }

void clEditor::DelayedSetActive() { CallAfter(&clEditor::SetActive); }

void clEditor::OnFocus(wxFocusEvent& event)
{
    m_isFocused = true;
    event.Skip();

    clCommandEvent focus_gained{ wxEVT_STC_GOT_FOCUS };
    EventNotifier::Get()->AddPendingEvent(focus_gained);
}

bool clEditor::IsFocused() const
{
#ifdef __WXGTK__
    // Under GTK, when popup menu is ON, we will receive a "FocusKill" event
    // which means that we lost the focus. So the IsFocused() method is using
    // either the m_isFocused flag or the m_popupIsOn flag
    return m_isFocused || m_popupIsOn;
#else
    return m_isFocused;
#endif
}

void clEditor::ShowCalltip(clCallTipPtr tip)
{
    GetFunctionTip()->AddCallTip(tip);
    GetFunctionTip()->Highlight(m_context->DoGetCalltipParamterIndex());

    // In an ideal world, we would like our tooltip to be placed
    // on top of the caret.
    wxPoint pt = PointFromPosition(GetCurrentPosition());
    GetFunctionTip()->Activate(pt, GetCurrLineHeight(), StyleGetBackground(wxSTC_C_DEFAULT), GetLexerId());
}

int clEditor::PositionAfterPos(int pos) { return wxStyledTextCtrl::PositionAfter(pos); }

int clEditor::GetCharAtPos(int pos) { return wxStyledTextCtrl::GetCharAt(pos); }

int clEditor::PositionBeforePos(int pos) { return wxStyledTextCtrl::PositionBefore(pos); }

std::vector<int> clEditor::GetChanges() { return m_deltas->GetChanges(); }

void clEditor::OnFindInFiles() { m_deltas->Clear(); }

void clEditor::OnHighlightWordChecked(wxCommandEvent& e)
{
    e.Skip();
// Mainly needed under Mac to toggle the
// buffered drawing on and off
#ifdef __WXMAC__
    SetBufferedDraw(e.GetInt() == 1 ? true : false);
    // clLogMessage("Settings buffered drawing to: %d", e.GetInt());
    if (e.GetInt()) {
        Refresh();
    }
#endif
}

void clEditor::OnKeyUp(wxKeyEvent& event)
{
    event.Skip();
    if (event.GetKeyCode() == WXK_CONTROL || event.GetKeyCode() == WXK_SHIFT || event.GetKeyCode() == WXK_ALT) {

        // Clear hyperlink markers
        SetIndicatorCurrent(INDICATOR_HYPERLINK);
        IndicatorClearRange(0, GetLength());
        m_hyperLinkIndicatroEnd = m_hyperLinkIndicatroStart = wxNOT_FOUND;

        // Clear debugger marker
        SetIndicatorCurrent(INDICATOR_DEBUGGER);
        IndicatorClearRange(0, GetLength());
    }
    UpdateLineNumbers(true);
}

size_t clEditor::GetCodeNavModifier()
{
    size_t mod = wxMOD_NONE;
    if (GetOptions()->HasOption(OptionsConfig::Opt_NavKey_Alt))
        mod |= wxMOD_ALT;
    if (GetOptions()->HasOption(OptionsConfig::Opt_NavKey_Control))
        mod |= wxMOD_CONTROL;
    if (GetOptions()->HasOption(OptionsConfig::Opt_NavKey_Shift))
        mod |= wxMOD_ALT;
    return mod;
}

void clEditor::OnFileFormatDone(wxCommandEvent& e)
{
    if (e.GetString() != FileUtils::RealPath(GetFileName().GetFullPath())) {
        // not this file
        e.Skip();
        return;
    }

    // Restore the markers
    DoRestoreMarkers();
}

void clEditor::OnFileFormatStarting(wxCommandEvent& e)
{
    if (e.GetString() != FileUtils::RealPath(GetFileName().GetFullPath())) {
        // not this file
        e.Skip();
        return;
    }
    DoSaveMarkers();
}

void clEditor::DoRestoreMarkers()
{
    MarkerDeleteAll(mmt_all_bookmarks);
    for (size_t i = smt_FIRST_BMK_TYPE; i < m_savedMarkers.size(); ++i) {
        MarkerAdd(m_savedMarkers.at(i).first, m_savedMarkers.at(i).second);
    }
    m_savedMarkers.clear();
    NotifyMarkerChanged();
}

void clEditor::DoSaveMarkers()
{
    m_savedMarkers.clear();
    int nLine = LineFromPosition(0);

    int nFoundLine = MarkerNext(nLine, mmt_all_bookmarks);
    while (nFoundLine >= 0) {
        for (size_t type = smt_FIRST_BMK_TYPE; type < smt_LAST_BMK_TYPE; ++type) {
            int mask = (1 << type);
            if (MarkerGet(nLine) & mask) {
                m_savedMarkers.push_back(std::make_pair(nFoundLine, type));
            }
        }
        nFoundLine = MarkerNext(nFoundLine + 1, mmt_all_bookmarks);
    }
}

void clEditor::ToggleBreakpointEnablement()
{
    int lineno = GetCurrentLine() + 1;

    BreakptMgr* bm = ManagerST::Get()->GetBreakpointsMgr();
    clDebuggerBreakpoint bp = bm->GetBreakpoint(FileUtils::RealPath(GetFileName().GetFullPath()), lineno);
    if (bp.IsNull())
        return;

    if (!bm->DelBreakpointByLineno(bp.file, bp.lineno))
        return;

    bp.is_enabled = !bp.is_enabled;
    bp.debugger_id = wxNOT_FOUND;
    bp.internal_id = bm->GetNextID();
    ManagerST::Get()->GetBreakpointsMgr()->AddBreakpoint(bp);
    clMainFrame::Get()->GetDebuggerPane()->GetBreakpointView()->Initialize();
}

void clEditor::DoUpdateTLWTitle(bool raise)
{
    // ensure that the top level window parent of this editor is 'Raised'
    wxWindow* tlw = ::wxGetTopLevelParent(this);

    if (!IsDetached()) {
        clMainFrame::Get()->SetFrameTitle(this);

    } else {
        wxString title;
        if (IsRemoteFile()) {
            title << GetRemotePath() << "[" << GetRemoteData()->GetAccountName() << "]";
        } else {
            title << FileUtils::RealPath(GetFileName().GetFullPath());
        }
        if (GetModify()) {
            title.Prepend(wxT(" \u25CF "));
        }
        tlw->SetLabel(title);
    }
}

bool clEditor::IsDetached() const
{
    const wxWindow* tlw = ::wxGetTopLevelParent(const_cast<clEditor*>(this));
    return (tlw && (clMainFrame::Get() != tlw));
}

int clEditor::GetPosAtMousePointer()
{
    wxPoint mousePtInScreenCoord = ::wxGetMousePosition();
    wxPoint clientPt = ScreenToClient(mousePtInScreenCoord);
    return PositionFromPoint(clientPt);
}

void clEditor::GetWordAtMousePointer(wxString& word, wxRect& wordRect)
{
    word.clear();
    wordRect = wxRect();

    long start = wxNOT_FOUND;
    long end = wxNOT_FOUND;
    if (GetSelectedText().IsEmpty()) {
        int pos = GetPosAtMousePointer();
        if (pos != wxNOT_FOUND) {
            start = WordStartPosition(pos, true);
            end = WordEndPosition(pos, true);
        }
    } else {
        start = GetSelectionStart();
        end = GetSelectionEnd();
    }

    wxFont font = StyleGetFont(0);
    wxClientDC dc(this);

    dc.SetFont(font);
    wxSize sz = dc.GetTextExtent(GetTextRange(start, end));
    wxPoint ptStart = PointFromPosition(start);
    wxRect rr(ptStart, sz);

    word = GetTextRange(start, end);
    wordRect = rr;
}

void clEditor::ShowTooltip(const wxString& tip, const wxString& title, int pos)
{
    DoShowCalltip(pos, title, tip, false);
}

void clEditor::ShowRichTooltip(const wxString& tip, const wxString& title, int pos)
{
    if (m_richTooltip)
        return;
    wxUnusedVar(pos);
    wxString word;
    wxRect rect;
    GetWordAtMousePointer(word, rect);
    m_richTooltip = new wxRichToolTip(title, tip);
    m_richTooltip->ShowFor(this, &rect);
}

wxString clEditor::GetFirstSelection()
{
    int nNumSelections = GetSelections();
    if (nNumSelections > 1) {
        for (int i = 0; i < nNumSelections; ++i) {
            int startPos = GetSelectionNStart(i);
            int endPos = GetSelectionNEnd(i);
            if (endPos > startPos) {
                return wxStyledTextCtrl::GetTextRange(startPos, endPos);
            }
        }
        // default
        return wxEmptyString;

    } else {
        return wxStyledTextCtrl::GetSelectedText();
    }
}

void clEditor::SetLineVisible(int lineno)
{
    int offsetFromTop = 10;
    if (lineno != wxNOT_FOUND) {
        // try this: set the first visible line to be -10 lines from
        // the requested lineNo
        lineno -= offsetFromTop;
        if (lineno < 0) {
            lineno = 0;
        }
        SetFirstVisibleLine(VisibleFromDocLine(lineno));
        // If the line is hidden - expand it
        EnsureVisible(lineno);
    }
}

void clEditor::DoWrapPrevSelectionWithChars(wxChar first, wxChar last)
{
    // Undo the previous action
    BeginUndoAction();

    // Restore the previous selection
    Undo();
    ClearSelections();

    int charsAdded(0);
    std::vector<std::pair<int, int>> selections;
    for (size_t i = 0; i < m_prevSelectionInfo.GetCount(); ++i) {
        int startPos, endPos;
        m_prevSelectionInfo.At(i, startPos, endPos);

        // insert the wrappers characters
        // Each time we add character into the document, we move the insertion
        // point by 1 (this is why charsAdded is used)
        startPos += charsAdded;
        InsertText(startPos, first);
        ++charsAdded;

        endPos += charsAdded;
        InsertText(endPos, last);
        ++charsAdded;

        selections.push_back(std::make_pair(startPos + 1, endPos));
    }

    // And select it
    for (size_t i = 0; i < selections.size(); ++i) {
        const std::pair<int, int>& range = selections.at(i);
        if (i == 0) {
            SetSelection(range.first, range.second);
        } else {
            AddSelection(range.first, range.second);
        }
    }
    EndUndoAction();
}

void clEditor::OnTimer(wxTimerEvent& event)
{
    event.Skip();
    m_timerHighlightMarkers->Start(100, true);
    if (!HasFocus())
        return;

    if (!HasSelection()) {
        HighlightWord(false);

    } else {
        if (EditorConfigST::Get()->GetInteger("highlight_word") == 1) {
            int pos = GetCurrentPos();
            int wordStartPos = WordStartPos(pos, true);
            int wordEndPos = WordEndPos(pos, true);
            wxString word = GetTextRange(wordStartPos, wordEndPos);

            // Read the primary selected text
            int mainSelectionStart = GetSelectionNStart(GetMainSelection());
            int mainSelectionEnd = GetSelectionNEnd(GetMainSelection());

            wxString selectedText = GetTextRange(mainSelectionStart, mainSelectionEnd);
            if (!m_highlightedWordInfo.IsValid(this)) {

                // Check to see if we have marker already on
                // we got a selection
                bool textMatches = (selectedText == word);
                if (textMatches) {
                    // No markers set yet
                    DoHighlightWord();

                } else if (!textMatches) {
                    // clear markers if the text does not match
                    HighlightWord(false);
                }
            } else {
                // we got the markers on, check that they still matches the highlighted word
                if (selectedText != m_highlightedWordInfo.GetWord()) {
                    HighlightWord(false);
                } else {
                    // clDEBUG1() << "Markers are valid - nothing more to be done" << clEndl;
                }
            }
        }
    }
}

void clEditor::SplitSelection()
{
    CHECK_COND_RET(HasSelection() && GetSelections() == 1);

    int selLineStart = LineFromPosition(GetSelectionStart());
    int selLineEnd = LineFromPosition(GetSelectionEnd());

    if (selLineEnd != selLineStart) {
        if (selLineStart > selLineEnd) {
            // swap
            std::swap(selLineEnd, selLineStart);
        }

        ClearSelections();
        for (int i = selLineStart; i <= selLineEnd; ++i) {
            int caretPos;
            if (i != GetLineCount() - 1) {
                // Normally use PositionBefore as LineEnd includes the EOL as well
                caretPos = PositionBefore(LineEnd(i));
            } else {
                caretPos = LineEnd(i); // but it seems not for the last line of the doc
            }
            if (i == selLineStart) {
                // first selection
                SetSelection(caretPos, caretPos);
            } else {
                AddSelection(caretPos, caretPos);
            }
        }
    }
}

void clEditor::CenterLinePreserveSelection(int line) { CallAfter(&clEditor::CenterLinePreserveSelectionAfter, line); }

void clEditor::CenterLinePreserveSelectionAfter(int line) { CenterLinePreserveSelection(this, line); }

void clEditor::CenterLinePreserveSelection(wxStyledTextCtrl* ctrl, int line)
{
    int selection_start = ctrl->GetSelectionStart();
    int selection_end = ctrl->GetSelectionEnd();

    clSTCHelper::CenterLine(ctrl, line, wxNOT_FOUND);

    if (selection_end != wxNOT_FOUND && selection_start != wxNOT_FOUND) {
        ctrl->SetSelection(selection_start, selection_end);
        scroll_range(ctrl, selection_start, selection_end);
    }
}

void clEditor::CenterLine(int line, int col) { clSTCHelper::CenterLine(this, line, col); }

void clEditor::OnEditorConfigChanged(wxCommandEvent& event)
{
    event.Skip();
    UpdateOptions();
    CallAfter(&clEditor::SetProperties);
    UpdateLineNumbers(true);
}

void clEditor::ConvertIndentToSpaces()
{
    clSTCLineKeeper lk(GetCtrl());
    bool useTabs = GetUseTabs();
    SetUseTabs(false);
    BeginUndoAction();
    int lineCount = GetLineCount();
    for (int i = 0; i < lineCount; ++i) {
        int indentStart = PositionFromLine(i);
        int indentEnd = GetLineIndentPosition(i);
        int lineIndentSize = GetLineIndentation(i);

        if (indentEnd > indentStart) {
            // this line have indentation
            // delete it
            DeleteRange(indentStart, indentEnd - indentStart);
            SetLineIndentation(i, lineIndentSize);
        }
    }
    EndUndoAction();
    SetUseTabs(useTabs);
}

void clEditor::ConvertIndentToTabs()
{
    clSTCLineKeeper lk(GetCtrl());
    bool useTabs = GetUseTabs();
    SetUseTabs(true);
    BeginUndoAction();
    int lineCount = GetLineCount();
    for (int i = 0; i < lineCount; ++i) {
        int indentStart = PositionFromLine(i);
        int indentEnd = GetLineIndentPosition(i);
        int lineIndentSize = GetLineIndentation(i);

        if (indentEnd > indentStart) {
            // this line have indentation
            // delete it
            DeleteRange(indentStart, indentEnd - indentStart);
            SetLineIndentation(i, lineIndentSize);
        }
    }
    EndUndoAction();
    SetUseTabs(useTabs);
}

void clEditor::DoCancelCodeCompletionBox()
{
    if (m_calltip) {
        m_calltip->Hide();
        m_calltip->Destroy();
        m_calltip = NULL;
    }
    // wxCodeCompletionBoxManager::Get().DestroyCCBox();
}

int clEditor::GetFirstSingleLineCommentPos(int from, int commentStyle)
{
    int lineNu = LineFromPos(from);
    int lastPos = from + LineLength(lineNu);
    for (int i = from; i < lastPos; ++i) {
        if (GetStyleAt(i) == commentStyle) {
            return i;
        }
    }
    return wxNOT_FOUND;
}

int clEditor::GetNumberFirstSpacesInLine(int line)
{
    int start = PositionFromLine(line);
    int lastPos = start + LineLength(line);
    for (int i = start; i < lastPos; ++i) {
        if (!isspace(GetCharAt(i))) {
            return i - start;
        }
    }
    return wxNOT_FOUND;
}

void clEditor::ToggleLineComment(const wxString& commentSymbol, int commentStyle)
{
    int start = GetSelectionStart();
    int end = GetSelectionEnd();

    if (start > end) {
        wxSwap(start, end);
    }

    int lineStart = LineFromPosition(start);
    int lineEnd = LineFromPosition(end);

    // Check if the "end" position is at the start of a line, in that case, don't
    // include it. Only do this in case of a selection.
    int endLineStartPos = PositionFromLine(lineEnd);
    if (lineStart < lineEnd && endLineStartPos == end) {
        --lineEnd;
    }

    bool indentedComments = GetOptions()->GetIndentedComments();

    bool doingComment;
    int indent = 0;
    if (indentedComments) {
        // Check if there is a comment in the line 'lineStart'
        int startCommentPos = GetFirstSingleLineCommentPos(PositionFromLine(lineStart), commentStyle);
        doingComment = (startCommentPos == wxNOT_FOUND);
        if (doingComment) {
            // Find the minimum indent (in whitespace characters) among all the selected lines
            // The comments will be indented with the found number of characters
            indent = 100000;
            bool indentFound = false;
            for (int i = lineStart; i <= lineEnd; i++) {
                int indentThisLine = GetNumberFirstSpacesInLine(i);
                if ((indentThisLine != wxNOT_FOUND) && (indentThisLine < indent)) {
                    indent = indentThisLine;
                    indentFound = true;
                }
            }
            if (!indentFound) {
                // Set the indent to zero in case of selection of empty lines
                indent = 0;
            }
        }
    } else {
        doingComment = (GetStyleAt(start) != commentStyle);
    }

    BeginUndoAction();
    for (; lineStart <= lineEnd; ++lineStart) {
        start = PositionFromLine(lineStart);
        if (doingComment) {
            if (indentedComments) {
                if (indent < LineLength(lineStart)) {
                    // Shift the position of the comment by the 'indent' number of characters
                    InsertText(start + indent, commentSymbol);
                }
            } else {
                InsertText(start, commentSymbol);
            }

        } else {
            int firstCommentPos = GetFirstSingleLineCommentPos(start, commentStyle);
            if (firstCommentPos != wxNOT_FOUND) {
                if (GetStyleAt(firstCommentPos) == commentStyle) {
                    SetAnchor(firstCommentPos);
                    SetCurrentPos(PositionAfter(PositionAfter(firstCommentPos)));
                    DeleteBackNotLine();
                }
            }
        }
    }
    EndUndoAction();

    SetCaretAt(PositionFromLine(lineEnd + 1));
    ChooseCaretX();
}

void clEditor::CommentBlockSelection(const wxString& commentBlockStart, const wxString& commentBlockEnd)
{
    const int start = GetSelectionStart();
    int end = GetSelectionEnd();
    if (LineFromPosition(PositionBefore(end)) != LineFromPosition(end)) {
        end = std::max(start, PositionBefore(end));
    }
    if (start == end)
        return;

    SetCurrentPos(end);

    BeginUndoAction();
    InsertText(end, commentBlockEnd);
    InsertText(start, commentBlockStart);
    EndUndoAction();

    CharRight();
    CharRight();
    ChooseCaretX();
}

void clEditor::QuickAddNext()
{
    if (!HasSelection()) {
        int start = WordStartPos(GetCurrentPos(), true);
        int end = WordEndPos(GetCurrentPos(), true);
        SetSelection(start, end);
        return;
    }

    int count = GetSelections();
    int start = GetSelectionNStart(count - 1);
    int end = GetSelectionNEnd(count - 1);
    if (GetSelections() == 1) {
        ClearSelections();
        SetSelection(start, end);
        SetMainSelection(0);
    }

    // Use the find flags of the quick find bar for this
    int searchFlags = clMainFrame::Get()->GetMainBook()->GetFindBar()->GetSearchFlags();

    wxString findWhat = GetTextRange(start, end);
    int where = this->FindText(end, GetLength(), findWhat, searchFlags);
    if (where != wxNOT_FOUND) {
        AddSelection(where + findWhat.length(), where);
        CenterLineIfNeeded(LineFromPos(where));
    }

    wxString message;
    message << _("Found and selected ") << GetSelections() << _(" matches");
    clGetManager()->GetStatusBar()->SetMessage(message);
}

void clEditor::QuickFindAll()
{
    if (GetSelections() != 1)
        return;

    int start = GetSelectionStart();
    int end = GetSelectionEnd();
    wxString findWhat = GetTextRange(start, end);
    if (findWhat.IsEmpty())
        return;

    ClearSelections();

    int matches(0);
    int firstMatch(wxNOT_FOUND);

    // Use the find flags of the quick find bar for this
    int searchFlags = clMainFrame::Get()->GetMainBook()->GetFindBar()->GetSearchFlags();
    CallAfter(&clEditor::SetFocus);

    int where = this->FindText(0, GetLength(), findWhat, searchFlags);
    while (where != wxNOT_FOUND) {
        if (matches == 0) {
            firstMatch = where;
            SetSelection(where, where + findWhat.length());
            SetMainSelection(0);
            CenterLineIfNeeded(LineFromPos(where));

        } else {
            AddSelection(where + findWhat.length(), where);
        }
        ++matches;
        where = this->FindText(where + findWhat.length(), GetLength(), findWhat, searchFlags);
    }
    wxString message;
    message << _("Found and selected ") << GetSelections() << _(" matches");
    clGetManager()->GetStatusBar()->SetMessage(message);
    if (firstMatch != wxNOT_FOUND) {
        SetMainSelection(0);
    }
}

void clEditor::CenterLineIfNeeded(int line, bool force)
{
    // ensure that this line is visible
    EnsureVisible(line);

    // Center this line
    int linesOnScreen = LinesOnScreen();
    if (force || ((line < GetFirstVisibleLine()) || (line > (GetFirstVisibleLine() + LinesOnScreen())))) {
        // To place our line in the middle, the first visible line should be
        // the: line - (linesOnScreen / 2)
        int firstVisibleLine = line - (linesOnScreen / 2);
        if (firstVisibleLine < 0) {
            firstVisibleLine = 0;
        }
        EnsureVisible(firstVisibleLine);
        SetFirstVisibleLine(firstVisibleLine);
    }
}

void clEditor::Print()
{
#if wxUSE_PRINTING_ARCHITECTURE
    if (g_printData == NULL) {
        g_printData = new wxPrintData();
        wxPrintPaperType* paper = wxThePrintPaperDatabase->FindPaperType(wxPAPER_A4);
        g_printData->SetPaperId(paper->GetId());
        g_printData->SetPaperSize(paper->GetSize());
        g_printData->SetOrientation(wxPORTRAIT);
        g_pageSetupData = new wxPageSetupDialogData();
        (*g_pageSetupData) = *g_printData;
        PageSetup();
    }

    // Black on White print mode
    SetPrintColourMode(wxSTC_PRINT_BLACKONWHITE);

    // No magnifications
    SetPrintMagnification(0);

    wxPrintDialogData printDialogData(*g_printData);
    wxPrinter printer(&printDialogData);
    clPrintout printout(this, FileUtils::RealPath(GetFileName().GetFullPath()));

    if (!printer.Print(this, &printout, true /*prompt*/)) {
        if (wxPrinter::GetLastError() == wxPRINTER_ERROR) {
            wxLogError(wxT("There was a problem printing. Perhaps your current printer is not set correctly?"));
        } else {
            clLogMessage(wxT("You canceled printing"));
        }
    } else {
        (*g_printData) = printer.GetPrintDialogData().GetPrintData();
    }
#endif // wxUSE_PRINTING_ARCHITECTURE
}

void clEditor::PageSetup()
{
#if wxUSE_PRINTING_ARCHITECTURE
    if (g_printData == NULL) {
        g_printData = new wxPrintData();
        wxPrintPaperType* paper = wxThePrintPaperDatabase->FindPaperType(wxPAPER_A4);
        g_printData->SetPaperId(paper->GetId());
        g_printData->SetPaperSize(paper->GetSize());
        g_printData->SetOrientation(wxPORTRAIT);
        g_pageSetupData = new wxPageSetupDialogData();
        (*g_pageSetupData) = *g_printData;
    }
    wxPageSetupDialog pageSetupDialog(this, g_pageSetupData);
    pageSetupDialog.ShowModal();
    (*g_printData) = pageSetupDialog.GetPageSetupData().GetPrintData();
    (*g_pageSetupData) = pageSetupDialog.GetPageSetupData();
#endif // wxUSE_PRINTING_ARCHITECTURE
}

void clEditor::OnMouseWheel(wxMouseEvent& event)
{
    event.Skip();
    if (::wxGetKeyState(WXK_CONTROL) && !GetOptions()->IsMouseZoomEnabled()) {
        event.Skip(false);
        return;
    } else if (IsCompletionBoxShown()) {
        event.Skip(false);
        // wxCodeCompletionBoxManager::Get().GetCCWindow()->DoMouseScroll(event);
    }
}

void clEditor::ApplyEditorConfig() { CallAfter(&clEditor::SetProperties); }

void clEditor::OpenURL(wxCommandEvent& event)
{
    wxString url = GetSelectedText();
    ::wxLaunchDefaultBrowser(url);
}

void clEditor::ReloadFromDisk(bool keepUndoHistory)
{
    wxWindowUpdateLocker locker(GetParent());
    SetReloadingFile(true);

    DoCancelCalltip();
    GetFunctionTip()->Deactivate();

    if (m_fileName.GetFullPath().IsEmpty() == true || !m_fileName.FileExists()) {
        SetEOLMode(GetEOLByOS());
        SetReloadingFile(false);
        return;
    }

    clEditorStateLocker stateLocker(GetCtrl());

    wxString text;
    bool file_read = false;
    m_fileBom.Clear();

    {
        wxBusyCursor bc; // io operation tends to be lengthy
#if USE_SFTP
        if (IsRemoteFile()) {
            wxMemoryBuffer content;
            if (!clSFTPManager::Get().AwaitReadFile(GetRemotePath(), GetRemoteData()->GetAccountName(), &content)) {
                wxString message;
                message << _("Failed to reload remote file: ") << GetRemotePath();
                wxMessageBox(message, "CodeLite", wxICON_WARNING | wxCENTRE | wxOK);
                return;
            }
            text = wxString((const unsigned char*)content.GetData(), wxConvUTF8, content.GetDataLen());
            file_read = true;
        }
#endif

        if (!file_read) {
            // Read the file we currently support:
            // BOM, Auto-Detect encoding & User defined encoding
            ReadFileWithConversion(m_fileName.GetFullPath(), text, GetOptions()->GetFileFontEncoding(), &m_fileBom);
        }
    }

    SetText(text);
    // clear the modified lines
    m_modifiedLines.clear();

    Colourise(0, wxNOT_FOUND);

    m_modifyTime = GetFileLastModifiedTime();
    SetSavePoint();

    UpdateOptions();
    CallAfter(&clEditor::SetProperties);

    if (!keepUndoHistory) {
        EmptyUndoBuffer();
        GetCommandsProcessor().Reset();
    }

    SetReloadingFile(false);

    // Notify about file-reload
    clCommandEvent e(wxEVT_FILE_LOADED);
    e.SetFileName(GetRemotePathOrLocal());
    EventNotifier::Get()->AddPendingEvent(e);
}

void clEditor::PreferencesChanged()
{
    m_statusBarFields = 0;
    if (clConfig::Get().Read(kConfigStatusbarShowLine, true)) {
        m_statusBarFields |= kShowLine;
    }
    if (clConfig::Get().Read(kConfigStatusbarShowColumn, true)) {
        m_statusBarFields |= kShowColumn;
    }
    if (clConfig::Get().Read(kConfigStatusbarShowLineCount, false)) {
        m_statusBarFields |= kShowLineCount;
    }
    if (clConfig::Get().Read(kConfigStatusbarShowPosition, false)) {
        m_statusBarFields |= kShowPosition;
    }
    if (clConfig::Get().Read(kConfigStatusbarShowLength, false)) {
        m_statusBarFields |= kShowLen;
    }
    if (clConfig::Get().Read(kConfigStatusbarShowSelectedChars, true)) {
        m_statusBarFields |= kShowSelectedChars;
    }
    if (clConfig::Get().Read(kConfigStatusbarShowSelectedLines, true)) {
        m_statusBarFields |= kShowSelectedLines;
    }
}

void clEditor::NotifyMarkerChanged(int lineNumber)
{
    // Notify about marker changes
    clCommandEvent eventMarker(wxEVT_MARKER_CHANGED);
    eventMarker.SetFileName(FileUtils::RealPath(GetFileName().GetFullPath()));
    if (lineNumber != wxNOT_FOUND) {
        eventMarker.SetLineNumber(lineNumber);
    }
    EventNotifier::Get()->AddPendingEvent(eventMarker);
}

wxString clEditor::GetWordAtPosition(int pos, bool wordCharsOnly)
{
    // Get the partial word that we have
    if (wordCharsOnly) {
        long start = WordStartPosition(pos, true);
        long end = WordEndPosition(pos, true);
        return GetTextRange(start, end);

    } else {
        int start = pos;
        int end = pos;
        int where = pos;
        // find the start pos
        while (true) {
            int p = PositionBefore(where);
            if ((p != wxNOT_FOUND) && IsWordChar(GetCharAt(p))) {
                where = p;
                if (where == 0) {
                    break;
                }
                continue;
            } else {
                break;
            }
        }
        wxSwap(start, where);
        end = WordEndPosition(pos, true);
        return GetTextRange(start, end);
    }
}

int clEditor::GetFirstNonWhitespacePos(bool backward)
{
    int from = GetCurrentPos();
    if (from == wxNOT_FOUND) {
        return wxNOT_FOUND;
    }

    int pos = from;
    if (backward) {
        from = PositionBefore(from);
    } else {
        from = PositionAfter(from);
    }
    while (from != wxNOT_FOUND) {
        wxChar ch = GetCharAt(from);
        switch (ch) {
        case ' ':
        case '\t':
        case '\n':
            return pos;
        default:
            break;
        }

        // Keep the previous location
        pos = from;

        // Move the position
        if (backward) {
            from = PositionBefore(from);
        } else {
            from = PositionAfter(from);
        }
    }
    return pos;
}

#ifdef __WXGTK__
#define MARING_SPACER 15
#else
#define MARING_SPACER 10
#endif

void clEditor::UpdateLineNumberMarginWidth()
{
    int new_width = log10(GetLineCount()) + 1;

    if (m_default_text_width == wxNOT_FOUND) {
        UpdateDefaultTextWidth();
    }

    int size = new_width * m_default_text_width + FromDIP(MARING_SPACER);
    SetMarginWidth(NUMBER_MARGIN_ID, GetOptions()->GetDisplayLineNumbers() ? size : 0);
}

void clEditor::OnZoom(wxStyledTextEvent& event)
{
    event.Skip();
    // When zooming, update the line number margin
    UpdateLineNumberMarginWidth();
    if (m_zoomProgrammatically) {
        m_zoomProgrammatically = false;
        return;
    }

    // User triggered this zoom
    int curzoom = GetZoom();

    auto editors = clMainFrame::Get()->GetMainBook()->GetAllEditors();

    for (auto editor : editors) {
        editor->SetZoomFactor(curzoom);
    }
}

void clEditor::DoToggleFold(int line, const wxString& textTag)
{
    ToggleFoldShowText(line, GetOptions()->GetUnderlineFoldLine() ? wxString() : textTag);
}

size_t clEditor::GetEditorTextRaw(std::string& text)
{
    text.clear();
    wxCharBuffer cb = GetTextRaw();
    if (cb.length()) {
        text.reserve(cb.length() + 1);
        text.append(cb.data());
    }
    return text.length();
}

wxString clEditor::GetRemotePathOrLocal() const
{
    if (IsRemoteFile()) {
        return GetRemotePath();
    } else {
        return FileUtils::RealPath(GetFileName().GetFullPath());
    }
}

wxString clEditor::GetRemotePath() const
{
    if (IsRemoteFile()) {
        return GetRemoteData()->GetRemotePath();
    }
    return wxEmptyString;
}

bool clEditor::IsRemoteFile() const { return GetRemoteData() != nullptr; }

SFTPClientData* clEditor::GetRemoteData() const
{
    auto cd = IEditor::GetClientData("sftp");
    if (cd) {
        return reinterpret_cast<SFTPClientData*>(cd);
    }
    return nullptr;
}

void clEditor::SetSemanticTokens(const wxString& classes,
                                 const wxString& variables,
                                 const wxString& methods,
                                 const wxString& others)
{
    wxString flatStrClasses = classes;
    wxString flatStrLocals = variables;
    wxString flatStrOthers = others;
    wxString flatStrMethods = methods;

    flatStrClasses.Trim().Trim(false);
    flatStrLocals.Trim().Trim(false);
    flatStrOthers.Trim().Trim(false);
    flatStrMethods.Trim().Trim(false);

    // locate the lexer
    auto lexer = ColoursAndFontsManager::Get().GetLexerForFile(FileUtils::RealPath(GetFileName().GetFullPath()));
    CHECK_PTR_RET(lexer);

    SetKeywordLocals(flatStrLocals);
    SetKeywordOthers(flatStrOthers);
    SetKeywordMethods(flatStrMethods);
    SetKeywordClasses(flatStrClasses);

    if (lexer->GetWordSet(LexerConf::WS_CLASS).is_ok()) {
        LOG_IF_DEBUG { clDEBUG1() << "Setting semantic tokens:" << endl; }
        lexer->ApplyWordSet(this, LexerConf::WS_CLASS, flatStrClasses);
        lexer->ApplyWordSet(this, LexerConf::WS_FUNCTIONS, flatStrMethods);
        lexer->ApplyWordSet(this, LexerConf::WS_VARIABLES, flatStrLocals);
        lexer->ApplyWordSet(this, LexerConf::WS_OTHERS, flatStrOthers);

    } else {
        LOG_IF_DEBUG { clDEBUG1() << "Setting semantic tokens (default):" << endl; }

        int keywords_class = wxNOT_FOUND;
        int keywords_variables = wxNOT_FOUND;

        switch (GetLexerId()) {
        case wxSTC_LEX_CPP:
            keywords_class = 1;
            keywords_variables = 3;
            break;

        case wxSTC_LEX_RUST:
            keywords_class = 3;
            keywords_variables = 4;
            break;

        case wxSTC_LEX_PYTHON:
            keywords_variables = 1;
            break;
        default:
            break;
        }
        if (!flatStrClasses.empty() && keywords_class != wxNOT_FOUND) {
            SetKeyWords(keywords_class, flatStrClasses);
            SetKeywordClasses(flatStrClasses);
        }

        if (!flatStrLocals.empty() && keywords_variables != wxNOT_FOUND) {
            SetKeyWords(keywords_variables, flatStrLocals);
            SetKeywordLocals(flatStrLocals);
        }
    }
    Colourise(0, wxSTC_INVALID_POSITION);
}

int clEditor::GetColumnInChars(int pos)
{
    int line = LineFromPosition(pos);
    int line_start_pos = PositionFromLine(line);
    return pos - line_start_pos;
}

void clEditor::SetZoomFactor(int zoom_factor)
{
    int curzoom = GetZoom();
    if (curzoom == zoom_factor) {
        return;
    }
    m_zoomProgrammatically = true;
    SetZoom(zoom_factor);
}

// ----------------------------------
// SelectionInfo
// ----------------------------------
struct SelectorSorter {
    bool operator()(const std::pair<int, int>& a, const std::pair<int, int>& b) { return a.first < b.first; }
};

void clEditor::SelectionInfo::Sort() { std::sort(this->selections.begin(), this->selections.end(), SelectorSorter()); }

void clEditor::DoSetCaretAt(long pos) { clEditor::DoSetCaretAt(this, pos); }

bool clEditor::HasBreakpointMarker(int line_number)
{
    int markers_bit_mask = MarkerGet(line_number);
    int mask = (1 << smt_breakpoint);
    return markers_bit_mask & mask;
}

size_t clEditor::GetBreakpointMarkers(std::vector<int>* lines)
{
    int mask = (1 << smt_breakpoint);
    int line = MarkerNext(0, mask);
    while (line != wxNOT_FOUND) {
        lines->push_back(line);
        line = MarkerNext(line + 1, mask);
    }
    return lines->size();
}

void clEditor::DeleteBreakpointMarkers(int line_number)
{
    // get a list of lines to work on
    std::vector<int> lines;
    if (line_number == wxNOT_FOUND) {
        GetBreakpointMarkers(&lines);
    } else {
        lines.push_back(line_number);
    }

    for (int line : lines) {
        MarkerDelete(line, smt_breakpoint);
    }
    m_breakpoints_tooltips.clear();
}

void clEditor::SetBreakpointMarker(int line_number, const wxString& tooltip)
{
    if (HasBreakpointMarker(line_number)) {
        m_breakpoints_tooltips.erase(line_number);
        m_breakpoints_tooltips.insert({ line_number, tooltip });
        return;
    }

    MarkerAdd(line_number, smt_breakpoint);
    m_breakpoints_tooltips.insert({ line_number, tooltip });
}

void clEditor::OnColoursAndFontsUpdated(clCommandEvent& event)
{
    event.Skip();
    UpdateDefaultTextWidth();
}

void clEditor::UpdateDefaultTextWidth() { m_default_text_width = TextWidth(wxSTC_STYLE_LINENUMBER, "X"); }

void clEditor::OnIdle(wxIdleEvent& event)
{
    if (!IsShown()) {
        return;
    }

    event.Skip();

    // The interval between idle events can not be under 250ms
    static clIdleEventThrottler event_throttler{ 100 };
    if (!event_throttler.CanHandle()) {
        return;
    }

    if (m_scrollbar_recalc_is_required) {
        m_scrollbar_recalc_is_required = false;
        RecalcHorizontalScrollbar();
    }

    // Optimization: do we need to update anything here?
    long current_pos = GetCurrentPosition();
    if (m_lastIdlePosition == current_pos) {
        // same position as last update, nothing to be done here
        return;
    }
    m_lastIdlePosition = current_pos;
    GetContext()->ProcessIdleActions();
}

void clEditor::ClearModifiedLines()
{
    // clear all modified lines
    m_clearModifiedLines = true;
}

void clEditor::OnModifiedExternally(clFileSystemEvent& event)
{
    event.Skip();
    if (event.GetFileName().empty() || (GetRemotePathOrLocal() == event.GetFileName())) {
        ReloadFromDisk(true); // keep file history
    }
}

void clEditor::OnActiveEditorChanged(wxCommandEvent& event)
{
    event.Skip();
    m_lastIdlePosition = wxNOT_FOUND; // reset the idle position
    IEditor* editor = clGetManager()->GetActiveEditor();
    if (editor->GetCtrl() != this) {
        return;
    }

    // Update line numbers drawings
    UpdateLineNumberMarginWidth();
    UpdateLineNumbers(true);
}

void clEditor::NotifyTextUpdated()
{
    // Use CallAfter
    CallAfter(&clEditor::DrawLineNumbers, true);
}

void clEditor::DrawLineNumbers(bool force)
{
    UpdateLineNumberMarginWidth();
    UpdateLineNumbers(force);
}

void clEditor::DoClearBraceHighlight()
{
    if (m_hasBraceHighlight) {
        m_hasBraceHighlight = false;
        wxStyledTextCtrl::BraceHighlight(wxSTC_INVALID_POSITION, wxSTC_INVALID_POSITION);
        wxStyledTextCtrl::SetHighlightGuide(0); // clear any indent lines highlight
    }
}

void clEditor::DoBraceMatching()
{
    if (!m_hightlightMatchedBraces) {
        DoClearBraceHighlight();
        return;
    }

    long current_position = GetCurrentPosition();
    if (HasSelection()) {
        DoClearBraceHighlight();
        return;
    }

    if (m_context->IsCommentOrString(PositionBefore(current_position))) {
        DoClearBraceHighlight();
        return;
    }

    int ch = SafeGetChar(current_position);
    static std::vector<int> braces = { '<', '>', '{', '}', '(', ')', '[', ']' };
    auto found = std::find_if(braces.begin(), braces.end(), [ch](const char c) { return c == ch; });
    if (found == braces.end()) {
        current_position = PositionBefore(current_position);
        ch = SafeGetChar(current_position);
        found = std::find_if(braces.begin(), braces.end(), [ch](const char c) { return c == ch; });
        if (found == braces.end()) {
            DoClearBraceHighlight();
            return;
        }
    }

    BraceMatch(current_position);
}
