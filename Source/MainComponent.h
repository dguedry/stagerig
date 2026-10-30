#pragma once

#include "Engine.h"
#include "KeyboardPanel.h"
#include "StagePanel.h"
#include "SongsPanel.h"
#include "RemoteServer.h"
#include "Hotspot.h"
#include "HelpWindow.h"
#include <juce_gui_extra/juce_gui_extra.h>

namespace perf
{

class InputsPanel;
class ProgramsPanel;
class SlotsPanel;
class MappingsPanel;

class MainComponent : public juce::Component,
                      private Engine::Listener,
                      private juce::Timer
{
public:
    /** @param initialSetup  a setup file to open instead of the last one (may be empty). */
    MainComponent (Engine&, juce::PropertiesFile& settings, const juce::File& initialSetup = {});
    ~MainComponent() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    /** Saves the current setup to its file (or the autosave file). Called on quit. */
    void saveOnQuit();

    int getSelectedInput() const            { return selectedInput; }
    int getEditedProgram() const;

    void openPluginEditor (int inputIndex, int program, int slot, int effect = -1);

    void showStatus (const juce::String&);
    /** Marks the setup as changed so the periodic autosave picks it up. */
    void markDirty()                        { dirty = true; }
    /** ARA-only plugins need an ARA host; they may show an empty editor and can hang. */
    static bool looksLikeAraPlugin (const juce::PluginDescription&);
    void selectInput (int);
    void loadSetupFile (const juce::File&);
    /** An instrument row was clicked: the on-screen keyboard plays that instrument. */
    void playInstrumentFromKeyboard (int slot);

private:
    // Engine::Listener
    void setupChanged() override;
    void outputsChanged() override;
    void programChanged (int inputIndex, int program) override;
    void programContentChanged (int inputIndex, int program) override;
    void learnReceived (int inputIndex, MappingDef::Source, int number) override;
    void tapTempoLearned (int cc) override;
    void parameterTouched (int inputIndex, int program, int slot, int effect, int paramIndex) override;
    void statusMessage (const juce::String&) override;

    void timerCallback() override;
    void toggleRecording();
    static juce::String formatElapsed (double seconds);

    void refreshAll();
    void refreshProgramView();

    // file handling
    void newSetup();
    void openSetup();
    void saveSetup (bool forceAskForFile);
    bool writeSetupFile (const juce::File&, int captureBudgetMs = -1);
    void setCurrentFile (const juce::File&);
    juce::File getAutosaveFile() const;

    void showAudioSettings();
    void showOutputs();
    void printProgramMap();
    void chooseGmSoundfont();
    std::unique_ptr<juce::FileChooser> gmChooser;
    void showTempoMenu();
    void updateTempoLabel();
    void showRemote();

    /** Starts the tablet-control server, without showing the dialog. Returns true
        when it is listening. Used at startup to restore the previous session's
        choice, where a dialog would be an unwanted interruption. */
    bool startRemote();
    void chooseHotspotAdapter (std::function<void (bool)> done);
    void startHotspot();
    void showPluginManager();

    Engine& engine;
    juce::PropertiesFile& settings;
    juce::File currentFile;
    int selectedInput = 0;

    // toolbar
    juce::TextButton newBtn { "New" }, openBtn { "Open..." }, saveBtn { "Save" }, saveAsBtn { "Save As..." },
                     audioBtn { "Audio..." }, pluginsBtn { "Plugins..." },
                     panicBtn { "PANIC" };
    juce::TextButton optionsBtn { "Options" };
    juce::TextButton keyboardBtn { "Keyboard" };
    juce::TextButton stageBtn { "Stage" };
    juce::TextButton performBtn { "Perform" };
    juce::TextButton songsBtn { "Songs" };
    juce::TextButton remoteBtn { "Tablet" };
    juce::TextButton helpBtn { "Help" };
    std::unique_ptr<KeyboardPanel> keyboardPanel;
    std::unique_ptr<StagePanel> stagePanel;
    std::unique_ptr<SongsPanel> songsPanel;

    /** A thin bar between two panes; dragging it resizes the pane on its left.
        The widths it sets are kept in the settings, so the window comes back
        the way it was left. */
    class PaneDivider : public juce::Component
    {
    public:
        std::function<void()> onBegin;
        std::function<void (int deltaX)> onDrag;
        std::function<void()> onRelease;
        PaneDivider() { setMouseCursor (juce::MouseCursor::LeftRightResizeCursor); setRepaintsOnMouseActivity (true); }
        void paint (juce::Graphics&) override;
        void mouseDown (const juce::MouseEvent& e) override { lastX = e.getScreenX(); if (onBegin != nullptr) onBegin(); }
        void mouseDrag (const juce::MouseEvent& e) override { const int x = e.getScreenX(); if (onDrag != nullptr && x != lastX) onDrag (x - lastX); lastX = x; }
        void mouseUp (const juce::MouseEvent&) override { if (onRelease != nullptr) onRelease(); }
    private:
        int lastX = 0;
    };
    PaneDivider songsDivider, inputsDivider, programsDivider;
    int songsWidth = 230, inputsWidth = 260, programsWidth = 260;
    void savePaneWidths();
    std::unique_ptr<RemoteServer> remote;
    juce::Label tailLabel { {}, "Tail" };
    juce::Slider tailSlider;

    // Tempo: a readout you can tap, because a tap tempo you cannot see is one
    // you cannot trust.
    /** Shows or hides the songs and set list panel; under Options, and a
        drag of its divider all the way left hides it too. */
    void setSongsPanelShown (bool show, bool byDrag = false);
    juce::TextButton tapButton { "Tap" };
    juce::Label tempoLabel;
    juce::Label statusLabel, cpuLabel, fileLabel;

    std::unique_ptr<InputsPanel> inputsPanel;
    std::unique_ptr<ProgramsPanel> programsPanel;
    std::unique_ptr<SlotsPanel> slotsPanel;
    std::unique_ptr<MappingsPanel> mappingsPanel;

    std::unique_ptr<juce::FileChooser> fileChooser;
    juce::String statusText;
    double statusTime = 0;
    bool dirty = false;
    double lastAutosaveTime = 0;
    static constexpr double autosaveIntervalMs = 60000.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};

} // namespace perf
