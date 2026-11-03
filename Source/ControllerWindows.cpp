#include "ControllerWindows.h"
#include "ParamPicker.h"
#include "PanelParams.h"
#include "RemotePlugin.h"

namespace perf
{
using namespace juce;

namespace
{
    const Colour bgPanel   { 0xff26282f };
    const Colour bgRow     { 0xff2e313a };
    const Colour textDim   { 0xff9aa0ab };

    String noteName (int n) { return MidiMessage::getMidiNoteName (n, true, true, 3); }

    struct NamedColour { const char* name; uint32 rgb; };
    const NamedColour keyColourChoices[] = {
        { "Orange", 0xff8a00 }, { "Red", 0xff3c3c }, { "Yellow", 0xffdc00 }, { "Green", 0x3cdc3c },
        { "Teal", 0x00e6c8 }, { "Blue", 0x00b4ff }, { "Violet", 0x5a64ff }, { "Pink", 0xff3cc8 }, { "White", 0xffffff },
    };

    Label& caption (Label& l, const String& text)
    {
        l.setText (text, dontSendNotification);
        l.setFont (FontOptions (11.0f, Font::bold));
        l.setColour (Label::textColourId, textDim);
        return l;
    }

    Component* launch (Component* content, const String& title, int w, int h)
    {
        content->setSize (w, h);
        DialogWindow::LaunchOptions o;
        o.content.setOwned (content);
        o.dialogTitle = title;
        o.dialogBackgroundColour = bgPanel;
        o.escapeKeyTriggersCloseButton = true;
        o.useNativeTitleBar = true;
        o.resizable = true;
        return o.launchAsync();
    }
}

//==============================================================================
namespace
{
class ControlMapEditor : public Component
{
public:
    ControlMapEditor (Engine& e, SurfaceHub& h, ControlMapLibrary& lib, int in, int prog, int sl, std::function<void()> ch)
        : engine (e), hub (h), library (lib), input (in), program (prog), slot (sl), changed (std::move (ch))
    {
        const auto& sd = slotDef();
        plugin = sd.plugin;

        addAndMakeVisible (title);
        title.setFont (FontOptions (17.0f, Font::bold));
        title.setText ("What your keyboard's knobs turn on " + plugin.name, dontSendNotification);
        addAndMakeVisible (explain);
        explain.setColour (Label::textColourId, textDim);
        {
            const auto& prog = engine.getSetup().inputs[(size_t) input].programs[(size_t) program];
            const auto progName = String (program).paddedLeft ('0', 3) + (prog.name.isNotEmpty() ? " \"" + prog.name + "\"" : String());
            explain.setText ("Pages of eight knobs, with names for a keyboard's screen, and colours for its keys: keyswitches, a kit's pads, the range "
                             "that plays. A setup belongs to " + plugin.name + ", so every program with it can use it; which one this instrument "
                             "uses is kept with program " + progName + ". For one knob or pedal in this program only, use the program's MIDI mappings.",
                             dontSendNotification);
        }

        addAndMakeVisible (caption (setupCap, "SETUP"));
        addAndMakeVisible (setupBox);
        setupBox.onChange = [this] { chooseSetup(); };
        for (auto* b : { &newBtn, &deleteBtn, &importBtn, &exportBtn, &saveBtn, &addPageBtn, &removePageBtn, &addKeysBtn })
            addAndMakeVisible (b);
        newBtn.onClick = [this] { newSetup(); };
        deleteBtn.onClick = [this] { deleteSetup(); };
        importBtn.onClick = [this] { importSetup(); };
        exportBtn.onClick = [this] { exportSetup(); };
        saveBtn.onClick = [this] { save(); };
        addPageBtn.onClick = [this] { storePage(); storeKeys(); edit.pages.push_back ({ "Page " + String ((int) edit.pages.size() + 1), {} }); page = (int) edit.pages.size() - 1; showMap(); touched(); };
        removePageBtn.onClick = [this]
        {
            if (edit.pages.empty()) return;
            storeKeys();
            edit.pages.erase (edit.pages.begin() + page);
            page = jmax (0, page - 1);
            showMap();
            touched();
        };
        addKeysBtn.onClick = [this] { storePage(); storeKeys(); edit.keys.push_back ({ 36, 47, keyColourChoices[edit.keys.size() % 9].rgb, {} }); showMap(); touched(); };
        saveBtn.setColour (TextButton::buttonColourId, Colour (0xff2b4f80));

        addAndMakeVisible (caption (nameCap, "NAME"));
        addAndMakeVisible (caption (authorCap, "MADE BY"));
        addAndMakeVisible (nameEd);
        addAndMakeVisible (authorEd);
        nameEd.onTextChange = authorEd.onTextChange = [this] { touched(); };
        nameEd.setTooltip ("What this setup is for: the plugin's name for the whole plugin, or the instrument inside it (\"Alicia's Keys\")");

        addAndMakeVisible (caption (pageCap, "KNOB PAGE"));
        addAndMakeVisible (pageBox);
        pageBox.onChange = [this] { if (pageBox.getSelectedId() > 0) { storePage(); page = pageBox.getSelectedId() - 1; showPage(); } };
        addAndMakeVisible (pageNameEd);
        pageNameEd.onTextChange = [this] { touched(); };
        pageNameEd.setTooltip ("The page's name, shown when you step to it");

        for (int k = 0; k < ControlPage::numKnobs; ++k)
        {
            auto& r = knobRows[(size_t) k];
            r.cap.setText ("Knob " + String (k + 1), dontSendNotification);
            r.cap.setColour (Label::textColourId, textDim);
            addAndMakeVisible (r.cap);
            addAndMakeVisible (r.param);
            addAndMakeVisible (r.label);
            r.label.setTextToShowWhenEmpty ("Name on the screen", textDim);
            r.param.onChange = [this] { touched(); };
            r.label.onTextChange = [this] { touched(); };
        }

        addAndMakeVisible (caption (keysCap, "KEYS"));
        addAndMakeVisible (keysView);
        keysView.setViewedComponent (&keysList, false);
        keysView.setScrollBarsShown (true, false);
        addAndMakeVisible (status);
        status.setColour (Label::textColourId, textDim);

        fillParams();
        fillSetups (sd.controlMap);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (12);
        title.setBounds (r.removeFromTop (24));
        explain.setBounds (r.removeFromTop (54));
        r.removeFromTop (6);

        auto row = r.removeFromTop (16);
        setupCap.setBounds (row);
        row = r.removeFromTop (26);
        exportBtn.setBounds (row.removeFromRight (80)); row.removeFromRight (4);
        importBtn.setBounds (row.removeFromRight (80)); row.removeFromRight (4);
        deleteBtn.setBounds (row.removeFromRight (70)); row.removeFromRight (4);
        newBtn.setBounds (row.removeFromRight (60)); row.removeFromRight (8);
        setupBox.setBounds (row);
        r.removeFromTop (10);

        row = r.removeFromTop (16);
        nameCap.setBounds (row.removeFromLeft (row.getWidth() / 2));
        authorCap.setBounds (row);
        row = r.removeFromTop (26);
        nameEd.setBounds (row.removeFromLeft (row.getWidth() / 2).withTrimmedRight (8));
        authorEd.setBounds (row);
        r.removeFromTop (10);

        row = r.removeFromTop (16);
        pageCap.setBounds (row);
        row = r.removeFromTop (26);
        removePageBtn.setBounds (row.removeFromRight (100)); row.removeFromRight (4);
        addPageBtn.setBounds (row.removeFromRight (90)); row.removeFromRight (8);
        pageBox.setBounds (row.removeFromLeft (row.getWidth() / 2).withTrimmedRight (8));
        pageNameEd.setBounds (row);
        r.removeFromTop (6);

        // The knobs in two columns of four, as most keyboards lay them out in a row of eight.
        auto knobArea = r.removeFromTop (4 * 30);
        auto left = knobArea.removeFromLeft (knobArea.getWidth() / 2).withTrimmedRight (8);
        for (int k = 0; k < ControlPage::numKnobs; ++k)
        {
            auto& col = k < 4 ? left : knobArea;
            auto line = col.removeFromTop (30).reduced (0, 2);
            auto& kr = knobRows[(size_t) k];
            kr.cap.setBounds (line.removeFromLeft (52));
            kr.label.setBounds (line.removeFromRight (jmin (130, line.getWidth() / 2)));
            line.removeFromRight (4);
            kr.param.setBounds (line);
        }
        r.removeFromTop (8);

        auto bottom = r.removeFromBottom (28);
        saveBtn.setBounds (bottom.removeFromRight (120));
        bottom.removeFromRight (8);
        status.setBounds (bottom);
        r.removeFromBottom (6);

        row = r.removeFromTop (24);
        addKeysBtn.setBounds (row.removeFromRight (110).reduced (0, 1));
        keysCap.setBounds (row);
        keysView.setBounds (r);
        layoutKeys();
    }

private:
    struct KnobRow { Label cap; ParamPicker param; TextEditor label; };
    struct KeyRow : public Component
    {
        TextEditor low, high, label;
        ComboBox colour;
        TextButton remove { "x" };
        KeyRow()
        {
            for (auto* c : std::initializer_list<Component*> { &low, &high, &label, &colour, &remove }) addAndMakeVisible (c);
            low.setTooltip ("The lowest key, as a note (C1, F#2) or a number, as the plugin hears it");
            high.setTooltip ("The highest key");
            label.setTextToShowWhenEmpty ("What these keys are (\"Articulations\")", textDim);
            for (int i = 0; i < (int) std::size (keyColourChoices); ++i) colour.addItem (keyColourChoices[i].name, i + 1);
        }
        void resized() override
        {
            auto r = getLocalBounds().reduced (0, 2);
            remove.setBounds (r.removeFromRight (26)); r.removeFromRight (6);
            low.setBounds (r.removeFromLeft (64)); r.removeFromLeft (4);
            high.setBounds (r.removeFromLeft (64)); r.removeFromLeft (6);
            colour.setBounds (r.removeFromLeft (100)); r.removeFromLeft (6);
            label.setBounds (r);
        }
    };

    const SlotDef& slotDef() const { return engine.getSetup().inputs[(size_t) input].programs[(size_t) program].slots[(size_t) slot]; }
    RemotePlugin* livePlugin() const { return engine.isPluginAlive (input, program, slot) ? engine.getPlugin (input, program, slot) : nullptr; }

    void fillParams()
    {
        auto* p = livePlugin();
        params = p != nullptr ? p->getParameters() : ParamInfoList();
        for (auto& r : knobRows)
        {
            r.param.clear (dontSendNotification);
            r.param.addItem ("(nothing)", 1);
            for (const auto& prm : params)
                if (prm.midiController < 0) r.param.addItem (prm, prm.index + 2);
        }
        if (p == nullptr) status.setText ("The plugin is not running, so its parameters cannot be listed. Load the program first.", dontSendNotification);
    }

    /** The setups for this plugin in the box; `select` by name, "-" for none. */
    void fillSetups (const String& select)
    {
        setupBox.clear (dontSendNotification);
        if (const auto* nks = library.findNks (plugin))
            setupBox.addItem ("None: " + (nks->vendor.isNotEmpty() ? nks->vendor + "'s" : String ("the maker's")) + " own pages, from its NKS presets", 1);
        else
            setupBox.addItem ("None: the front panel's knobs, then every parameter", 1);
        const auto maps = library.forPlugin (plugin);
        names.clear();
        for (const auto* m : maps) names.add (m->name);
        for (int i = 0; i < names.size(); ++i) setupBox.addItem (names[i], 10 + i);
        int id = 1;
        if (select == "-") id = 1;
        else if (select.isEmpty()) id = names.isEmpty() ? 1 : 10;
        else if (names.indexOf (select, true) >= 0) id = 10 + names.indexOf (select, true);
        setupBox.setSelectedId (id, dontSendNotification);
        loadSelected();
    }

    void loadSelected()
    {
        const int id = setupBox.getSelectedId();
        hasMap = id >= 10;
        original = {};
        edit = {};
        if (hasMap)
            if (const auto* m = library.find (plugin, names[id - 10])) { original = *m; edit = *m; }
        page = 0;
        dirty = false;
        showMap();
    }

    void chooseSetup()
    {
        const int id = setupBox.getSelectedId();
        const String chosen = id >= 10 ? names[id - 10] : String ("-");
        if (dirty && hasMap) save();           // nothing typed is lost by looking at another
        engine.setSlotControlMap (input, program, slot, chosen);
        hub.libraryChanged();
        if (changed) changed();
        fillSetups (chosen);
    }

    void showMap()
    {
        for (auto* c : std::initializer_list<Component*> { &nameEd, &authorEd, &pageBox, &pageNameEd, &addPageBtn, &removePageBtn, &addKeysBtn, &saveBtn, &deleteBtn, &exportBtn })
            c->setEnabled (hasMap);
        nameEd.setText (edit.name, dontSendNotification);
        authorEd.setText (edit.author, dontSendNotification);
        pageBox.clear (dontSendNotification);
        for (int i = 0; i < (int) edit.pages.size(); ++i)
            pageBox.addItem (String (i + 1) + ". " + edit.pages[(size_t) i].name, i + 1);
        page = jlimit (0, jmax (0, (int) edit.pages.size() - 1), page);
        pageBox.setSelectedId (edit.pages.empty() ? 0 : page + 1, dontSendNotification);
        removePageBtn.setEnabled (hasMap && ! edit.pages.empty());
        showPage();

        keyRows.clear();
        for (const auto& z : edit.keys)
        {
            auto row = std::make_unique<KeyRow>();
            row->low.setText (noteName (z.low), dontSendNotification);
            row->high.setText (noteName (z.high), dontSendNotification);
            row->label.setText (z.label, dontSendNotification);
            int pick = 1;
            for (int i = 0; i < (int) std::size (keyColourChoices); ++i) if (keyColourChoices[i].rgb == z.colour) pick = i + 1;
            row->colour.setSelectedId (pick, dontSendNotification);
            auto* raw = row.get();
            row->low.onTextChange = row->high.onTextChange = row->label.onTextChange = [this] { touched(); };
            row->colour.onChange = [this] { touched(); };
            row->remove.onClick = [this, raw]
            {
                storePage();
                storeKeys();
                for (size_t i = 0; i < keyRows.size(); ++i)
                    if (keyRows[i].get() == raw) { edit.keys.erase (edit.keys.begin() + (long) i); break; }
                MessageManager::callAsync ([sp = SafePointer<ControlMapEditor> (this)] { if (sp != nullptr) { sp->showMap(); sp->touched(); } });
            };
            keysList.addAndMakeVisible (*row);
            keyRows.push_back (std::move (row));
        }
        layoutKeys();
        if (livePlugin() == nullptr) status.setText ("The plugin is not running, so its parameters cannot be listed. Load the program first.", dontSendNotification);
        else if (! hasMap) status.setText ("No setup: the knobs turn the front panel's picks, then every parameter eight at a time.", dontSendNotification);
        else if (! dirty) status.setText (edit.notes, dontSendNotification);
    }

    void showPage()
    {
        const bool any = hasMap && page < (int) edit.pages.size();
        pageNameEd.setText (any ? edit.pages[(size_t) page].name : String(), dontSendNotification);
        pageNameEd.setEnabled (any);
        for (int k = 0; k < ControlPage::numKnobs; ++k)
        {
            auto& r = knobRows[(size_t) k];
            ControlKnob knob;
            if (any && k < (int) edit.pages[(size_t) page].knobs.size()) knob = edit.pages[(size_t) page].knobs[(size_t) k];
            int id = 1;
            for (const auto& prm : params)
                if ((knob.paramId.isNotEmpty() && prm.id == knob.paramId) || (knob.paramId.isEmpty() && knob.paramName.isNotEmpty() && prm.name.equalsIgnoreCase (knob.paramName)))
                { id = prm.index + 2; break; }
            r.param.setSelectedId (id, dontSendNotification);
            r.label.setText (knob.label, dontSendNotification);
            r.param.setEnabled (any && ! params.empty());
            r.label.setEnabled (any);
        }
    }

    /** What the page's knobs show into the setup being edited. */
    void storePage()
    {
        if (! hasMap || page >= (int) edit.pages.size()) return;
        auto& p = edit.pages[(size_t) page];
        p.name = pageNameEd.getText().trim();
        if (p.name.isEmpty()) p.name = "Page " + String (page + 1);
        p.knobs.clear();
        for (int k = 0; k < ControlPage::numKnobs; ++k)
        {
            const auto& r = knobRows[(size_t) k];
            ControlKnob knob;
            const int idx = r.param.getSelectedId() - 2;
            if (idx >= 0 && idx < (int) params.size()) { knob.paramId = params[(size_t) idx].id; knob.paramName = params[(size_t) idx].name; }
            knob.label = r.label.getText().trim();
            p.knobs.push_back (knob);
        }
        // Knobs left empty at the end are not written.
        while (! p.knobs.empty() && p.knobs.back().isEmpty() && p.knobs.back().label.isEmpty()) p.knobs.pop_back();
    }

    void storeKeys()
    {
        if (! hasMap) return;
        for (size_t i = 0; i < keyRows.size() && i < edit.keys.size(); ++i)
        {
            auto& z = edit.keys[i];
            const int lo = parseNoteName (keyRows[i]->low.getText()), hi = parseNoteName (keyRows[i]->high.getText());
            if (lo >= 0) z.low = lo;
            if (hi >= 0) z.high = hi;
            if (z.high < z.low) std::swap (z.low, z.high);
            const int c = keyRows[i]->colour.getSelectedId() - 1;
            if (c >= 0 && c < (int) std::size (keyColourChoices)) z.colour = keyColourChoices[c].rgb;
            z.label = keyRows[i]->label.getText().trim();
        }
    }

    void layoutKeys()
    {
        const int w = jmax (200, keysView.getWidth() - keysView.getScrollBarThickness());
        keysList.setSize (w, jmax (30, (int) keyRows.size() * 30));
        for (size_t i = 0; i < keyRows.size(); ++i) keyRows[i]->setBounds (0, (int) i * 30, w, 30);
    }

    void touched()
    {
        if (! hasMap) return;
        dirty = true;
        status.setText ("Not saved yet", dontSendNotification);
    }

    void save()
    {
        if (! hasMap) return;
        storePage();
        storeKeys();
        edit.name = nameEd.getText().trim();
        edit.author = authorEd.getText().trim();
        if (edit.name.isEmpty()) edit.name = plugin.name;
        if (! edit.name.equalsIgnoreCase (original.name))
        {
            for (const auto* m : library.forPlugin (plugin))
                if (m->name.equalsIgnoreCase (edit.name)) { status.setText ("There is already a setup called " + edit.name + " for " + plugin.name + ".", dontSendNotification); return; }
            if (original.pluginName.isNotEmpty()) library.remove (original);     // renamed: the old file goes
        }
        if (const auto r = library.save (edit); r.failed()) { status.setText (r.getErrorMessage(), dontSendNotification); return; }
        engine.setSlotControlMap (input, program, slot, edit.name);
        hub.libraryChanged();
        if (changed) changed();
        dirty = false;
        fillSetups (edit.name);
        status.setText ("Saved. Every program with " + plugin.name + " can use it.", dontSendNotification);
    }

    void newSetup()
    {
        if (dirty && hasMap) save();
        ControlMap m;
        m.pluginName = plugin.name;
        m.pluginManufacturer = plugin.manufacturerName;
        m.name = plugin.name;
        for (int n = 2; library.find (plugin, m.name) != nullptr; ++n) m.name = plugin.name + " " + String (n);
        // It starts as the knobs are now -- the maker's pages, or the front panel's -- so there is something to change rather than eight blanks.
        const auto* nks = library.findNks (plugin);
        if (auto* p = livePlugin(); p != nullptr && nks != nullptr)
        {
            const auto made = controlMapFromNks (*nks, plugin, p->getParameters());
            m.pages = made.pages;
            m.author = made.author;
            m.notes = made.notes;
        }
        ControlPage first;
        first.name = "Sound";
        if (auto* p = livePlugin(); p != nullptr && m.pages.empty())
        {
            const auto& sd = slotDef();
            std::vector<int> picks;
            if (sd.panelParams.empty()) picks = pickPanelParams (p->getParameters(), ControlPage::numKnobs);
            else for (const auto& id : sd.panelParams) if (const int i = p->findParameterIndex (id); i >= 0) picks.push_back (i);
            for (int idx : picks)
                if ((int) first.knobs.size() < ControlPage::numKnobs)
                    first.knobs.push_back ({ p->getParameters()[(size_t) idx].id, p->getParameters()[(size_t) idx].name, {} });
        }
        if (m.pages.empty()) m.pages.push_back (first);
        if (const auto r = library.save (m); r.failed()) { status.setText (r.getErrorMessage(), dontSendNotification); return; }
        engine.setSlotControlMap (input, program, slot, m.name);
        hub.libraryChanged();
        if (changed) changed();
        fillSetups (m.name);
        status.setText (m.author.isNotEmpty() && nks != nullptr ? "A new setup, from " + m.author + "'s pages, for you to change."
                                                                 : String ("A new setup, from the knobs as they were. Name it after the instrument if the plugin holds many."),
                        dontSendNotification);
    }

    void deleteSetup()
    {
        if (! hasMap) return;
        const auto doomed = original;
        auto sp = SafePointer<ControlMapEditor> (this);
        AlertWindow::showOkCancelBox (AlertWindow::WarningIcon, "Delete setup", "Delete the setup " + doomed.name + " for " + plugin.name + "?\n\nIts file goes from this computer. There is no undo.",
                                      "Delete", "Keep", nullptr, ModalCallbackFunction::create ([sp, doomed] (int r)
        {
            if (sp == nullptr || r == 0) return;
            sp->library.remove (doomed);
            sp->engine.setSlotControlMap (sp->input, sp->program, sp->slot, {});
            sp->hub.libraryChanged();
            if (sp->changed) sp->changed();
            sp->dirty = false;
            sp->fillSetups ({});
        }));
    }

    void importSetup()
    {
        chooser = std::make_unique<FileChooser> ("Import a controller setup", File::getSpecialLocation (File::userHomeDirectory).getChildFile ("Downloads"),
                                                 String ("*") + ControlMap::fileExtension + ";*.json");
        chooser->launchAsync (FileBrowserComponent::openMode | FileBrowserComponent::canSelectFiles, [sp = SafePointer<ControlMapEditor> (this)] (const FileChooser& fc)
        {
            if (sp == nullptr || fc.getResult() == File()) return;
            ControlMap m;
            if (const auto r = sp->library.importFile (fc.getResult(), &m); r.failed()) { sp->status.setText (r.getErrorMessage(), dontSendNotification); return; }
            sp->hub.libraryChanged();
            if (! m.appliesTo (sp->plugin))
            {
                sp->status.setText ("Imported " + m.name + ", a setup for " + m.pluginName + ": it is there for that plugin's instruments.", dontSendNotification);
                return;
            }
            sp->engine.setSlotControlMap (sp->input, sp->program, sp->slot, m.name);
            if (sp->changed) sp->changed();
            sp->fillSetups (m.name);
            sp->status.setText ("Imported " + m.name + (m.author.isNotEmpty() ? ", made by " + m.author : String()) + ".", dontSendNotification);
        });
    }

    void exportSetup()
    {
        if (! hasMap) return;
        if (dirty) save();
        const auto fileName = library.fileFor (edit).getFileName();
        chooser = std::make_unique<FileChooser> ("Export the controller setup", File::getSpecialLocation (File::userDocumentsDirectory).getChildFile (fileName),
                                                 String ("*") + ControlMap::fileExtension);
        chooser->launchAsync (FileBrowserComponent::saveMode | FileBrowserComponent::warnAboutOverwriting, [sp = SafePointer<ControlMapEditor> (this)] (const FileChooser& fc)
        {
            if (sp == nullptr || fc.getResult() == File()) return;
            auto f = fc.getResult();
            if (! f.hasFileExtension (ControlMap::fileExtension)) f = f.withFileExtension (ControlMap::fileExtension);
            const auto r = sp->edit.saveToFile (f);
            sp->status.setText (r.wasOk() ? "Exported to " + f.getFullPathName() + ". Whoever has " + sp->plugin.name + " can import it." : r.getErrorMessage(), dontSendNotification);
        });
    }

    Engine& engine;
    SurfaceHub& hub;
    ControlMapLibrary& library;
    const int input, program, slot;
    std::function<void()> changed;
    PluginDescription plugin;
    ParamInfoList params;
    StringArray names;
    ControlMap original, edit;
    bool hasMap = false, dirty = false;
    int page = 0;

    Label title, explain, setupCap, nameCap, authorCap, pageCap, keysCap, status;
    ComboBox setupBox, pageBox;
    TextButton newBtn { "New" }, deleteBtn { "Delete" }, importBtn { "Import..." }, exportBtn { "Export..." }, saveBtn { "Save" },
               addPageBtn { "+ Page" }, removePageBtn { "- This page" }, addKeysBtn { "+ Keys" };
    TextEditor nameEd, authorEd, pageNameEd;
    std::array<KnobRow, ControlPage::numKnobs> knobRows;
    Viewport keysView;
    Component keysList;
    std::vector<std::unique_ptr<KeyRow>> keyRows;
    std::unique_ptr<FileChooser> chooser;
};

//==============================================================================
class ControllersView : public Component, private Timer
{
public:
    ControllersView (Engine& e, SurfaceHub& h, int selected, std::function<void()> ch) : engine (e), hub (h), changed (std::move (ch))
    {
        addAndMakeVisible (caption (foundCap, "KEYBOARDS STAGERIG TALKS TO"));
        addAndMakeVisible (found);
        found.setJustificationType (Justification::topLeft);
        addAndMakeVisible (launchkeyBtn);
        addAndMakeVisible (kontrolBtn);
        addAndMakeVisible (kontrolDawBtn);
        kontrolDawBtn.setButtonText ("Komplete Kontrol S MK2 and MK3, A and M: program names on its screen, its track buttons choose them");
        kontrolDawBtn.setToggleState (hub.isKontrolDawEnabled(), dontSendNotification);
        kontrolDawBtn.onClick = [this] { hub.setKontrolDawEnabled (kontrolDawBtn.getToggleState()); };
        kontrolDawBtn.setTooltip ("Through the keyboard's DAW port, in Native Instruments' DAW protocol. An MK3 has the port itself; an MK2's, an A's "
                                  "and an M's is made by NI's Host Integration, on Windows and macOS.");
        launchkeyBtn.setButtonText ("Novation Launchkey MK4: its screen shows the program and the knobs, its knobs turn them");
        kontrolBtn.setButtonText ("Komplete Kontrol S MK1: its light guide shows the instruments' keys (experimental, Linux)");
        launchkeyBtn.setToggleState (hub.isLaunchkeyEnabled(), dontSendNotification);
        kontrolBtn.setToggleState (hub.isKompleteKontrolEnabled(), dontSendNotification);
        launchkeyBtn.onClick = [this] { hub.setLaunchkeyEnabled (launchkeyBtn.getToggleState()); };
        kontrolBtn.onClick = [this] { hub.setKompleteKontrolEnabled (kontrolBtn.getToggleState()); };
        kontrolBtn.setTooltip ("Worked out from the keyboard itself, not from a published description. A Komplete Kontrol has been seen to drop off "
                               "the USB while this was on and need a full power-down to come back; the keys kept playing throughout.");

        addAndMakeVisible (caption (inputCap, "KNOBS AND BUTTONS OF"));
        addAndMakeVisible (inputBox);
        const auto& inputs = engine.getSetup().inputs;
        for (int i = 0; i < (int) inputs.size(); ++i)
            if (inputs[(size_t) i].kind == InputDef::Kind::midi) inputBox.addItem (inputs[(size_t) i].name, i + 1);
        inputBox.setSelectedId (selected >= 0 && selected < (int) inputs.size() && inputs[(size_t) selected].kind == InputDef::Kind::midi ? selected + 1 : 0, dontSendNotification);
        if (inputBox.getSelectedId() == 0 && inputBox.getNumItems() > 0) inputBox.setSelectedItemIndex (0, dontSendNotification);
        inputBox.onChange = [this] { hub.cancelLearn(); showControls(); };

        addAndMakeVisible (explain);
        explain.setColour (Label::textColourId, textDim);
        explain.setText ("Which knob and button on this keyboard is which, in every program. The knobs of the keyboards above work by themselves; "
                         "for any other, press Learn and turn the knob. Buttons are taught the same way, on any keyboard. What the knobs turn "
                         "is each plugin's: its Keyboard button.", dontSendNotification);

        for (int i = 0; i < numRows; ++i)
        {
            auto& r = rows[(size_t) i];
            r.name.setText (i < SurfaceControls::numKnobs ? "Knob " + String (i + 1) : String (SurfaceControls::actionName (i - SurfaceControls::numKnobs)), dontSendNotification);
            addAndMakeVisible (r.name);
            addAndMakeVisible (r.what);
            r.what.setColour (Label::textColourId, textDim);
            addAndMakeVisible (r.learn);
            addAndMakeVisible (r.forget);
            r.learn.onClick = [this, i] { learn (i); };
            r.forget.onClick = [this, i] { forget (i); };
        }
        addAndMakeVisible (relativeBtn);
        relativeBtn.setButtonText ("The knobs are endless: they send steps around 64, not where they are");
        relativeBtn.onClick = [this]
        {
            if (current() < 0) return;
            auto c = engine.getSetup().inputs[(size_t) current()].controls;
            c.knobsRelative = relativeBtn.getToggleState();
            engine.setInputControls (current(), c);
            if (changed) changed();
        };
        showControls();
        showFound();
        startTimer (500);
    }

    ~ControllersView() override { hub.cancelLearn(); }

    void resized() override
    {
        auto r = getLocalBounds().reduced (12);
        foundCap.setBounds (r.removeFromTop (16));
        found.setBounds (r.removeFromTop (60));
        launchkeyBtn.setBounds (r.removeFromTop (24));
        kontrolBtn.setBounds (r.removeFromTop (24));
        kontrolDawBtn.setBounds (r.removeFromTop (24));
        r.removeFromTop (14);
        inputCap.setBounds (r.removeFromTop (16));
        inputBox.setBounds (r.removeFromTop (26).removeFromLeft (260));
        r.removeFromTop (4);
        explain.setBounds (r.removeFromTop (52));
        r.removeFromTop (4);
        auto left = r.removeFromLeft (r.getWidth() / 2).withTrimmedRight (10);
        for (int i = 0; i < numRows; ++i)
        {
            auto& col = i < SurfaceControls::numKnobs ? left : r;
            auto line = col.removeFromTop (28).reduced (0, 2);
            auto& row = rows[(size_t) i];
            row.forget.setBounds (line.removeFromRight (60)); line.removeFromRight (4);
            row.learn.setBounds (line.removeFromRight (70)); line.removeFromRight (6);
            row.name.setBounds (line.removeFromLeft (i < SurfaceControls::numKnobs ? 60 : 130));
            row.what.setBounds (line);
        }
        left.removeFromTop (4);
        relativeBtn.setBounds (left.removeFromTop (44));
    }

private:
    static constexpr int numRows = SurfaceControls::numKnobs + SurfaceControls::numActions;
    struct Row { Label name, what; TextButton learn { "Learn" }, forget { "Forget" }; };

    int current() const { return inputBox.getSelectedId() - 1; }

    void showControls()
    {
        const int in = current();
        const bool any = in >= 0 && in < (int) engine.getSetup().inputs.size();
        const auto c = any ? engine.getSetup().inputs[(size_t) in].controls : SurfaceControls();
        for (int i = 0; i < numRows; ++i)
        {
            auto& r = rows[(size_t) i];
            String what;
            if (i < SurfaceControls::numKnobs) what = c.knobCC[(size_t) i] > 0 ? "Controller " + String (c.knobCC[(size_t) i]) : String ("not taught");
            else
            {
                const auto& id = c.buttons[(size_t) (i - SurfaceControls::numKnobs)];
                what = id.isNotEmpty() ? SurfaceHub::describeControl (id) : String ("not taught");
            }
            r.what.setText (learning == i ? String ("Waiting: turn or press it now...") : what, dontSendNotification);
            r.what.setColour (Label::textColourId, learning == i ? Colours::orange : textDim);
            r.learn.setButtonText (learning == i ? "Cancel" : "Learn");
            r.learn.setEnabled (any);
            r.forget.setEnabled (any);
        }
        relativeBtn.setToggleState (c.knobsRelative, dontSendNotification);
        relativeBtn.setEnabled (any);
    }

    void showFound()
    {
        String text;
        const auto& inputs = engine.getSetup().inputs;
        for (const auto& s : hub.getSurfaces())
        {
            text << s.name << "   on " << (s.input >= 0 && s.input < (int) inputs.size() ? inputs[(size_t) s.input].name : String ("no input"));
            StringArray has;
            has.add ("knobs and buttons");
            if (s.screen) has.add ("screen");
            if (s.lights) has.add ("light guide");
            text << "   (" << has.joinIntoString (", ") << ")\n";
        }
        for (const auto& p : hub.getProblems()) text << p << "\n";
        if (text.isEmpty()) text = "None plugged in. Any keyboard still plays; its knobs and buttons can be taught below.";
        if (text != found.getText()) found.setText (text.trimEnd(), dontSendNotification);
    }

    void learn (int row)
    {
        if (learning == row) { hub.cancelLearn(); learning = -1; showControls(); return; }
        learning = row;
        const int what = row < SurfaceControls::numKnobs ? row : 100 + (row - SurfaceControls::numKnobs);
        hub.learn (current(), what, [sp = SafePointer<ControllersView> (this)] (const String&)
        {
            if (sp == nullptr) return;
            sp->learning = -1;
            sp->showControls();
            if (sp->changed) sp->changed();
        });
        showControls();
    }

    void forget (int row)
    {
        if (current() < 0) return;
        auto c = engine.getSetup().inputs[(size_t) current()].controls;
        if (row < SurfaceControls::numKnobs) c.knobCC[(size_t) row] = 0;
        else c.buttons[(size_t) (row - SurfaceControls::numKnobs)].clear();
        engine.setInputControls (current(), c);
        if (changed) changed();
        showControls();
    }

    void timerCallback() override
    {
        showFound();
        if (learning >= 0 && ! hub.isLearning()) { learning = -1; showControls(); }
    }

    Engine& engine;
    SurfaceHub& hub;
    std::function<void()> changed;
    int learning = -1;
    Label foundCap, found, inputCap, explain;
    ToggleButton launchkeyBtn, kontrolBtn, kontrolDawBtn, relativeBtn;
    ComboBox inputBox;
    std::array<Row, (size_t) numRows> rows;
};
}

Component* showControlMapEditor (Engine& engine, SurfaceHub& hub, ControlMapLibrary& library, int input, int program, int slot, std::function<void()> changed)
{
    const auto& inputs = engine.getSetup().inputs;
    if (input < 0 || input >= (int) inputs.size() || program < 0 || program >= InputDef::numPrograms) return nullptr;
    const auto& prog = inputs[(size_t) input].programs[(size_t) program];
    if (slot < 0 || slot >= (int) prog.slots.size() || ! prog.slots[(size_t) slot].isPlugin()) return nullptr;
    return launch (new ControlMapEditor (engine, hub, library, input, program, slot, std::move (changed)),
            "Keyboard knobs: " + prog.slots[(size_t) slot].plugin.name, 820, 658);
}

Component* showControllers (Engine& engine, SurfaceHub& hub, int selectedInput, std::function<void()> changed)
{
    return launch (new ControllersView (engine, hub, selectedInput, std::move (changed)), "Your keyboards", 860, 510);
}

} // namespace perf
