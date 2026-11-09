#pragma once

#include <juce_core/juce_core.h>

namespace perf::DeviceReservation
{

/** Sound cards StageRig opens directly (the ALSA type), asked for from whatever
    holds them through the desktop's device reservation (the D-Bus protocol
    JACK and Ardour use: org.freedesktop.ReserveDevice1.AudioN). PipeWire's
    session manager owns every card's reservation and lets go of the card when
    asked, closing it; without that, opening a card PipeWire had open waited
    for ever, and the Audio window froze with it.

    Holds exactly these cards (by ALSA card index) from now on: new ones are
    asked for, ones no longer wanted handed back. Then waits, up to
    waitMs, for every one of them to be closed by its old owner. Returns the
    cards still open by someone else after that (empty when all are free).
    Linux only; elsewhere does nothing. Message thread. */
juce::Array<int> holdOnly (const juce::Array<int>& cards, int waitMs = 3000);

/** Hands every card back (the ALSA type left, or StageRig quitting). The
    reservations also end by themselves if StageRig dies. */
inline void releaseAll() { holdOnly ({}, 0); }

/** True when the card's sound devices are all closed, as far as the kernel knows. */
bool cardIsClosed (int card);

} // namespace perf::DeviceReservation
