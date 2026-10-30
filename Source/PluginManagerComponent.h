#pragma once

#include "PluginHost.h"
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_extra/juce_gui_extra.h>

namespace perf
{

/**
    The Plugins window: JUCE's plugin table plus our own scan controls. Scans run in
    the app-owned PluginScanner with progress shown inline, so there is no popup that
    can land behind the main window and closing this window never interrupts a scan.
*/
class PluginManagerComponent : public juce::Component,
                               private juce::ChangeListener,
                               private juce::Timer
{
public:
    PluginManagerComponent (PluginHost& h, std::function<void (const juce::String&)> statusCallback = {})
        : host (h), onStatus (std::move (statusCallback)),
          list (h.getFormatManager(), h.getKnownPlugins(), h.getDeadMansPedalFile(), &h.getSettings(), true)
    {
        addAndMakeVisible (list);
        list.getOptionsButton().setVisible (false);     // replaced by the buttons below

        for (auto* format : host.getFormatManager().getFormats())
        {
            auto* b = scanButtons.add (new juce::TextButton ("Scan " + format->getName()));
            b->setTooltip ("Look for new or updated " + format->getName() + " plugins in:\n" + host.getScanPaths (*format).toString().replace (";", "\n"));
            b->onClick = [this, format] { queue.push_back ({ format, host.getScanPaths (*format) }); startNext(); };
            addAndMakeVisible (b);
        }

        addAndMakeVisible (folderBtn);
        folderBtn.setTooltip ("Add a folder to the scan paths and scan it for every format");
        folderBtn.onClick = [this] { chooseFolder(); };

        addAndMakeVisible (removeBtn);
        removeBtn.setTooltip ("Remove the selected plugin: it stays off, out of later scans too. Or switch back on one of the switched-off ones listed below");
        removeBtn.onClick = [this] { removeSelected(); };

        addAndMakeVisible (clearBtn);
        clearBtn.setTooltip ("Forget every plugin, including the ones switched off after hanging");
        clearBtn.onClick = [this] { host.getKnownPlugins().clear(); host.getKnownPlugins().clearBlacklistedFiles(); host.saveKnownPlugins(); status ("List cleared. Scan again to rebuild it."); };

        addAndMakeVisible (cancelBtn);
        cancelBtn.onClick = [this] { queue.clear(); host.getScanner().cancel(); status ("Cancelling: waiting for the plugins being tested to finish..."); };

        addAndMakeVisible (progress);
        addAndMakeVisible (statusLabel);
        statusLabel.setFont (juce::FontOptions (13.0f));
        statusLabel.setJustificationType (juce::Justification::centredLeft);
        statusLabel.setMinimumHorizontalScale (0.7f);

        host.getScanner().addChangeListener (this);
        updateScanState();
    }

    ~PluginManagerComponent() override
    {
        host.getScanner().removeChangeListener (this);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (8);
        auto bottom = r.removeFromBottom (24);
        r.removeFromBottom (6);
        auto buttons = r.removeFromBottom (26);
        r.removeFromBottom (6);
        list.setBounds (r);

        for (auto* b : scanButtons) { b->setBounds (buttons.removeFromLeft (110)); buttons.removeFromLeft (6); }
        folderBtn.setBounds (buttons.removeFromLeft (120)); buttons.removeFromLeft (18);
        clearBtn.setBounds (buttons.removeFromRight (90)); buttons.removeFromRight (6);
        removeBtn.setBounds (buttons.removeFromRight (130));

        cancelBtn.setBounds (bottom.removeFromRight (90)); bottom.removeFromRight (6);
        progress.setBounds (bottom.removeFromLeft (220)); bottom.removeFromLeft (8);
        statusLabel.setBounds (bottom);
    }

private:
    struct QueuedScan { juce::AudioPluginFormat* format; juce::FileSearchPath paths; };

    void startNext()
    {
        if (host.getScanner().isScanning() || queue.empty()) return;
        auto next = queue.front();
        queue.erase (queue.begin());
        if (! host.getScanner().startScan (*next.format, next.paths))
            status ("A scan is already running.");
        updateScanState();
    }

    void chooseFolder()
    {
        chooser = std::make_unique<juce::FileChooser> ("Choose a folder to scan for plugins",
                                                       juce::File::getSpecialLocation (juce::File::userHomeDirectory));
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                              [this] (const juce::FileChooser& fc)
        {
            auto folder = fc.getResult();
            if (! folder.isDirectory()) return;
            host.addScanFolder (folder);
            for (auto* format : host.getFormatManager().getFormats())
                queue.push_back ({ format, juce::FileSearchPath (folder.getFullPathName()) });
            startNext();
        });
    }

    void removeSelected()
    {
        auto& kpl = host.getKnownPlugins();
        const int row = list.getTableListBox().getSelectedRow();
        if (row < 0) { status ("Select a plugin in the list first."); return; }
        const auto types = kpl.getTypes();
        if (row < types.size())
        {
            /* Off the list, and off the next scan too: a plugin removed by hand
               -- one with no licence, one that puts up a dialog -- must not
               come back the next time the folder is scanned. It sits in the
               switched-off rows below, where Remove switches it back on. */
            status ("Removed " + types[row].name + ". It stays off until you select it below and switch it back on.");
            kpl.addToBlacklist (types[row].fileOrIdentifier);
            for (const auto& t : kpl.getTypes()) if (t.fileOrIdentifier == types[row].fileOrIdentifier) kpl.removeType (t);
        }
        else
        {
            const int b = row - types.size();
            auto files = kpl.getBlacklistedFiles();
            if (b < files.size())
            {
                kpl.removeFromBlacklist (files[b]);
                status ("Re-enabled " + juce::File (files[b]).getFileName() + ". Scan again to pick it up.");
            }
        }
        host.saveKnownPlugins();
    }

    void changeListenerCallback (juce::ChangeBroadcaster*) override { updateScanState(); }
    void timerCallback() override { progressValue = host.getScanner().getProgress(); }

    void updateScanState()
    {
        auto& scanner = host.getScanner();
        const bool scanning = scanner.isScanning();
        for (auto* b : scanButtons) b->setEnabled (! scanning || ! queue.empty());
        folderBtn.setEnabled (true);
        cancelBtn.setEnabled (scanning || ! queue.empty());
        progress.setVisible (scanning);

        if (scanning)
        {
            startTimer (100);
            auto current = scanner.getCurrentFile();
            statusLabel.setText ("Scanning " + scanner.getFormatName() + (current.isNotEmpty() ? ":  " + current : juce::String()), juce::dontSendNotification);
            statusLabel.setColour (juce::Label::textColourId, juce::Colours::white);
        }
        else
        {
            stopTimer();
            progressValue = 0.0;
            if (wasScanning)
            {
                juce::String msg = "Scan finished: " + juce::String (host.getKnownPlugins().getNumTypes()) + " plugins known.";
                auto failed = scanner.getFailedFiles();
                auto blacklisted = scanner.getNewlyBlacklistedFiles();
                auto removed = scanner.getRemovedPlugins();
                if (! removed.isEmpty())
                    msg << "  Removed (files gone): " << removed.joinIntoString (", ") << ".";
                if (! blacklisted.isEmpty())
                    msg << "  Switched off (hung while being tested): " << blacklisted.joinIntoString (", ");
                if (! failed.isEmpty())
                {
                    juce::StringArray described;
                    for (auto& f : failed)
                    {
                        const auto missing = PluginHost::brokenBridgeTarget (f);
                        described.add (juce::File (f).getFileName()
                                       + (missing.isNotEmpty() ? " (yabridge link points to a missing file: " + missing + " -- reinstall the plugin, then run yabridgectl sync)" : juce::String()));
                    }
                    msg << "  Not loadable: " << described.joinIntoString (", ");
                }
                status (msg);
                statusLabel.setColour (juce::Label::textColourId, blacklisted.isEmpty() && failed.isEmpty() ? juce::Colours::lightgreen : juce::Colours::orange);
            }
            startNext();
        }
        wasScanning = scanning;
    }

    void status (const juce::String& s)
    {
        statusLabel.setText (s, juce::dontSendNotification);
        if (onStatus) onStatus (s);
    }

    PluginHost& host;
    std::function<void (const juce::String&)> onStatus;
    juce::PluginListComponent list;
    juce::OwnedArray<juce::TextButton> scanButtons;
    juce::TextButton folderBtn { "Scan folder..." }, removeBtn { "Remove selected" }, clearBtn { "Clear list" }, cancelBtn { "Cancel" };
    double progressValue = 0.0;
    juce::ProgressBar progress { progressValue };
    juce::Label statusLabel;
    std::vector<QueuedScan> queue;
    std::unique_ptr<juce::FileChooser> chooser;
    bool wasScanning = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginManagerComponent)
};

} // namespace perf
