#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <cstdlib>

/** The one palette, and the one way a dialog is opened. Every window and
    panel draws from here, so a dialog opened from the songs panel is the
    same grey as one opened from Options. */
namespace perf::theme
{
inline const juce::Colour bgDark    { 0xff1e1f24 };   // the window's ground
inline const juce::Colour bgPanel   { 0xff26282f };   // a panel on it, and every dialog
inline const juce::Colour bgRow     { 0xff2e313a };   // a row on a panel
inline const juce::Colour bgPane    { 0xff1b1d22 };   // the songs pane, set into the ground rather than raised from it
inline const juce::Colour bgPaneRow { 0xff23262d };
inline const juce::Colour accent    { 0xff4f9dff };
inline const juce::Colour accentDim { 0xff2b4f80 };
inline const juce::Colour textDim   { 0xff9aa0ab };

/** Opens a dialog the way every dialog here opens: the panel colour, the
    system's title bar, Escape closes it. The content is owned by the window;
    a size of 0 keeps the one the content already has. */
/** A window title, marked when this is a separate copy (its own settings folder,
    STAGERIG_SETTINGS_DIR) so it is never taken for the rig playing beside it. */
inline juce::String windowTitle (const juce::String& title)
{
    return std::getenv ("STAGERIG_SETTINGS_DIR") != nullptr ? title + " (separate copy)" : title;
}

inline juce::DialogWindow* launchDialog (juce::Component* content, const juce::String& title, int w, int h, bool resizable = true)
{
    if (w > 0 && h > 0) content->setSize (w, h);
    juce::DialogWindow::LaunchOptions o;
    o.content.setOwned (content);
    o.dialogTitle = windowTitle (title);
    o.dialogBackgroundColour = bgPanel;
    o.escapeKeyTriggersCloseButton = true;
    o.useNativeTitleBar = true;
    o.resizable = resizable;
    return o.launchAsync();
}
}
