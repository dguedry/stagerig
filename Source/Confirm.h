#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>

namespace perf
{

/** One question before something that takes minutes to rebuild and has no
    undo: an input and its programs, a program, an instrument, a set list, the
    whole setup. `yes` is the verb on the button ("Remove", "Delete"). */
inline void confirm (const juce::String& title, const juce::String& body, const juce::String& yes, std::function<void()> then)
{
    auto* w = new juce::AlertWindow (title, body, juce::MessageBoxIconType::QuestionIcon);
    w->addButton (yes, 1, juce::KeyPress (juce::KeyPress::returnKey));
    w->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    w->enterModalState (true, juce::ModalCallbackFunction::create ([w, then] (int r)
    {
        std::unique_ptr<juce::AlertWindow> owned (w);
        if (r == 1 && then) then();
    }), false);
}

} // namespace perf
