#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <cstdlib>
#include <utility>
#include <vector>

/** The one palette, and the one way a dialog is opened. Every window and
    panel draws from here, so a dialog opened from the songs panel is the
    same grey as one opened from Options. */
namespace perf::theme
{
/* The colours every window draws from. Blue is for what you work -- buttons
   that are on, sliders, where typing goes. Amber is for what is live now --
   the program a keyboard is on, the song playing. They are variables, not
   constants, because the player chooses a look (Options): applyLook() sets
   them once at the start, before any window is made, and they are not changed
   after. The tablet page is given the same ones (RemoteServer.cpp). */
struct Look
{
    juce::String name;
    juce::Colour bgDark,      // the window's ground
                 bgPanel,     // a panel on it, and every dialog
                 bgRow,       // a row on a panel
                 bgRaised,    // a button, a box to type in
                 bgPane,      // the songs pane, set into the ground rather than raised from it
                 bgPaneRow,
                 border,
                 accent, accentDim,
                 amber, amberDim,   // live now; amberDim is a row lit in it, under white words
                 gold,
                 text, textDim,
                 selected,    // the row chosen in a list
                 heading;     // the small headings over the lists
    bool glow = false;        // the tablet's blue and amber haze behind its header
};

/** The looks that come with StageRig. The first is the one a new rig has. */
inline std::vector<Look> builtInLooks()
{
    using C = juce::Colour;
    return {
        // Calm: a near-black that is only faintly blue, softer blue and amber, amber kept for what is live.
        { "Calm", C (0xff0b0e13), C (0xff12161d), C (0xff1a2029), C (0xff232b37), C (0xff080a0e), C (0xff0f1318),
          C (0xff2a3442), C (0xff3d8fe6), C (0xff1f4573), C (0xffe39a3c), C (0xff7a5220), C (0xffc9a45c),
          C (0xfff2efea), C (0xff98a2ae), C (0xff1f4573), C (0xffc9d3df), false },
        // Vivid: the wallpaper's own picture -- midnight blue, electric blue and the amber of "Rig", headings and chosen rows in amber.
        { "Vivid", C (0xff000712), C (0xff001025), C (0xff0d1c33), C (0xff10294d), C (0xff00040b), C (0xff000c1d),
          C (0xff1a3555), C (0xff0087fc), C (0xff0a3770), C (0xfff08a14), C (0xff8a4a08), C (0xffcea04a),
          C (0xfffaf4eb), C (0xff9aa9bc), C (0xff8a4a08), C (0xfff08a14), true },
        // Classic: plain dark grey with one blue, as StageRig was before it had colours of its own.
        { "Classic", C (0xff1e1f24), C (0xff26282f), C (0xff2e313a), C (0xff373b46), C (0xff1b1d22), C (0xff23262d),
          C (0xff3c404b), C (0xff4f9dff), C (0xff2b4f80), C (0xffe0a030), C (0xff6e5320), C (0xffcea04a),
          C (0xffffffff), C (0xff9aa0ab), C (0xff2b4f80), C (0xff4f9dff), false },
    };
}

/** The built-in looks, then any the player made: a text file in the "Looks"
    folder beside the settings, named for the look, with lines such as
    `accent #3d8fe6`. A line `from Vivid` starts from that look (Calm
    otherwise), so a file needs only the colours it changes. */
inline std::vector<Look> allLooks (const juce::File& settingsFolder)
{
    auto looks = builtInLooks();
    static const std::pair<const char*, juce::Colour Look::*> names[] = {
        { "bgDark", &Look::bgDark }, { "bgPanel", &Look::bgPanel }, { "bgRow", &Look::bgRow }, { "bgRaised", &Look::bgRaised },
        { "bgPane", &Look::bgPane }, { "bgPaneRow", &Look::bgPaneRow }, { "border", &Look::border }, { "accent", &Look::accent },
        { "accentDim", &Look::accentDim }, { "amber", &Look::amber }, { "amberDim", &Look::amberDim }, { "gold", &Look::gold },
        { "text", &Look::text }, { "textDim", &Look::textDim }, { "selected", &Look::selected }, { "heading", &Look::heading } };

    auto files = settingsFolder.getChildFile ("Looks").findChildFiles (juce::File::findFiles, false, "*.txt");
    files.sort();
    for (const auto& f : files)
    {
        Look look = looks.front();
        juce::StringArray lines;
        f.readLines (lines);
        for (const auto& raw : lines)
        {
            const auto key = raw.trim().upToFirstOccurrenceOf (" ", false, false);
            const auto value = raw.trim().fromFirstOccurrenceOf (" ", false, false).trim();
            if (key == "from")
            {
                for (const auto& b : builtInLooks()) if (b.name.equalsIgnoreCase (value)) look = b;
            }
            else if (key == "glow")
                look.glow = value.equalsIgnoreCase ("yes");
            else if (const auto hex = value.trimCharactersAtStart ("#"); hex.length() == 6 && hex.containsOnly ("0123456789abcdefABCDEF"))
                for (const auto& [name, member] : names)
                    if (key == name) look.*member = juce::Colour ((juce::uint32) (0xff000000u | (juce::uint32) hex.getHexValue32()));
        }
        look.name = f.getFileNameWithoutExtension();
        bool builtIn = false;
        for (const auto& b : looks) builtIn = builtIn || b.name.equalsIgnoreCase (look.name);
        if (! builtIn) looks.push_back (look);
    }
    return looks;
}

/** The look of that name, or the first one when there is none of that name. */
inline Look findLook (const juce::File& settingsFolder, const juce::String& name)
{
    const auto looks = allLooks (settingsFolder);
    for (const auto& l : looks) if (l.name.equalsIgnoreCase (name)) return l;
    return looks.front();
}

inline juce::Colour bgDark, bgPanel, bgRow, bgRaised, bgPane, bgPaneRow, border, accent, accentDim,
                    amber, amberDim, gold, text, textDim, selected, heading;

/** Sets the colours above. Once, at the start: windows read them as they are made. */
inline void applyLook (const Look& l)
{
    bgDark = l.bgDark; bgPanel = l.bgPanel; bgRow = l.bgRow; bgRaised = l.bgRaised; bgPane = l.bgPane; bgPaneRow = l.bgPaneRow;
    border = l.border; accent = l.accent; accentDim = l.accentDim; amber = l.amber; amberDim = l.amberDim; gold = l.gold;
    text = l.text; textDim = l.textDim; selected = l.selected; heading = l.heading;
}

/* Until a look is chosen the colours are the first look's, so nothing is ever drawn with none. */
inline const bool lookSet = (applyLook (builtInLooks().front()), true);

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
