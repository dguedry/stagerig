#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <cstdlib>

/** The one palette, and the one way a dialog is opened. Every window and
    panel draws from here, so a dialog opened from the songs panel is the
    same grey as one opened from Options. */
namespace perf::theme
{
/* StageRig's colours, from its wallpaper and colour guide: a stage that is
   nearly black, lit in blue and in amber. Blue is for what you work --
   buttons that are on, sliders, where typing goes. Amber is for what is live
   now -- the program a keyboard is on, the song playing, the row chosen --
   and for the headings, as in the name. The tablet page's are the same
   (RemoteServer.cpp). */
inline const juce::Colour bgDark    { 0xff000712 };   // Midnight Black: the window's ground
inline const juce::Colour bgPanel   { 0xff001025 };   // Deep Navy: a panel on it, and every dialog
inline const juce::Colour bgRow     { 0xff0d1c33 };   // Slate Navy: a row on a panel
inline const juce::Colour bgRaised  { 0xff10294d };   // a button, a box to type in
inline const juce::Colour bgPane    { 0xff00040b };   // the songs pane, set into the ground rather than raised from it
inline const juce::Colour bgPaneRow { 0xff000c1d };
inline const juce::Colour border    { 0xff1a3555 };   // Panel Border
inline const juce::Colour accent    { 0xff0087fc };   // Electric Cyan
inline const juce::Colour accentDim { 0xff0a3770 };   // Signal Blue
inline const juce::Colour amber     { 0xfff08a14 };   // the amber of "Rig" in the name
inline const juce::Colour amberDim  { 0xff8a4a08 };   // a row lit in it, under white words
inline const juce::Colour gold      { 0xffcea04a };   // Warm Gold
inline const juce::Colour text      { 0xfffaf4eb };   // Warm White
inline const juce::Colour textDim   { 0xff9aa9bc };   // Muted Blue Gray

/** The same colours for everything JUCE draws by itself: buttons, boxes, menus, sliders. */
inline juce::LookAndFeel_V4::ColourScheme colourScheme()
{
    return { bgDark,                    // windowBackground
             bgRaised,                  // widgetBackground
             bgPanel,                   // menuBackground
             border.brighter (0.25f),   // outline
             text,                      // defaultText
             accent,                    // defaultFill
             text,                      // highlightedText
             accentDim,                 // highlightedFill
             text };                    // menuText
}

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
