#pragma once

#include "QrCode.h"
#include "Model.h"
#include <juce_core/juce_core.h>

namespace perf
{

/** The printable program map: which number plays which sound.

    When you pick programs by number on the keyboard, the thing you need is a
    sheet of paper listing the numbers you have filled in. JUCE cannot print on
    Linux, so this writes a self-contained HTML file styled for paper; the
    browser's print dialog does the rest. Only non-empty programs are listed,
    because the point is a short sheet you can scan quickly. */
struct ProgramMap
{
    static juce::String escape (const juce::String& t)
    {
        return t.replace ("&", "&amp;").replace ("<", "&lt;").replace (">", "&gt;");
    }

    /** What each program contains, in a few words: the instrument names. */
    static juce::String summarise (const ProgramDef& p)
    {
        juce::StringArray names;
        for (const auto& s : p.slots)
            if (s.plugin.name.isNotEmpty()) names.addIfNotAlreadyThere (s.plugin.name);
        return names.joinIntoString (" + ");
    }

    /** `tabletUrl`, when given, adds a scannable code to the sheet: tape it to the
        keyboard and a tablet reaches the program selector without anyone typing an
        address. */
    static juce::String toHtml (const Setup& setup, const juce::String& title,
                                const juce::String& tabletUrl = {})
    {
        juce::String h;
        h << "<!DOCTYPE html>\n<html><head><meta charset=\"utf-8\">\n"
          << "<title>" << escape (title) << " - program map</title>\n<style>\n"
          << "  body { font-family: system-ui, sans-serif; margin: 18mm 14mm; color: #000; }\n"
          << "  h1 { font-size: 15pt; margin: 0 0 2mm; }\n"
          << "  .sub { font-size: 9pt; color: #555; margin-bottom: 6mm; }\n"
          << "  h2 { font-size: 12pt; margin: 6mm 0 2mm; border-bottom: 1.5pt solid #000; padding-bottom: 1mm; }\n"
          << "  table { width: 100%; border-collapse: collapse; }\n"
          << "  td { padding: 1.2mm 2mm; border-bottom: 0.4pt solid #bbb; vertical-align: baseline; }\n"
          << "  td.n { font-weight: 700; font-size: 13pt; width: 14mm; font-variant-numeric: tabular-nums; }\n"
          << "  td.name { font-size: 12pt; }\n"
          << "  td.what { font-size: 9pt; color: #555; text-align: right; }\n"
          << "  .none { font-size: 10pt; color: #777; font-style: italic; }\n"
          << "  td.grp { font-size: 8.5pt; font-weight: 700; letter-spacing: .06em; text-transform: uppercase;\n"
          << "           color: #444; padding-top: 3mm; border-bottom: 0.8pt solid #888; }\n"
          << "  .qr { float: right; margin: -14mm 0 2mm 4mm; text-align: center; }\n"
          << "  .qrcap { font-size: 7.5pt; color: #555; margin-top: 1mm; line-height: 1.3; }\n"
          << "  @media print { body { margin: 10mm; } h2 { break-after: avoid; } tr { break-inside: avoid; } }\n"
          << "</style></head><body>\n"
          << "<h1>" << escape (title) << "</h1>\n<div class=\"sub\">Program map";
        h << " &middot; send these numbers as MIDI Program Change</div>\n";

        // A scannable code, so the sheet taped to the keyboard is also the way
        // in to the tablet selector: no address to read out or type.
        if (tabletUrl.isNotEmpty())
            if (const auto qr = QrCode::encode (tabletUrl); qr.isValid())
                h << "<div class=\"qr\">" << qr.toSvg (28)
                  << "<div class=\"qrcap\">Scan to pick programs<br>from a tablet</div></div>\n";

        for (const auto& in : setup.inputs)
        {
            h << "<h2>" << escape (in.name);
            if (in.midiDeviceName.isNotEmpty())
                h << " <span style=\"font-weight:400;font-size:9pt;color:#555\">"
                  << escape (in.midiDeviceName) << (in.channel > 0 ? ", channel " + juce::String (in.channel) : juce::String (", all channels")) << "</span>";
            h << "</h2>\n";

            /* Grouped, but never reordered: the numbers must stay in order because
               that is what you send from the keyboard, and a sheet where 007 sits
               above 003 is a sheet you cannot read at a glance. Groups appear as
               headings over runs, in the order they first occur. */
            juce::StringArray order;
            for (int i = 0; i < InputDef::numPrograms; ++i)
            {
                const auto& p = in.programs[(size_t) i];
                if (p.isEmpty() && p.name.isEmpty()) continue;
                order.addIfNotAlreadyThere (p.group);
            }

            juce::String rows;
            for (const auto& gname : order)
            {
                if (order.size() > 1)
                    rows << "<tr><td class=\"grp\" colspan=\"3\">"
                         << escape (gname.isNotEmpty() ? gname : "Other") << "</td></tr>\n";

                for (int i = 0; i < InputDef::numPrograms; ++i)
                {
                    const auto& p = in.programs[(size_t) i];
                    if (p.isEmpty() && p.name.isEmpty()) continue;
                    if (p.group != gname) continue;
                    rows << "<tr><td class=\"n\">" << juce::String (i).paddedLeft ('0', 3) << "</td>"
                         << "<td class=\"name\">" << escape (p.name.isNotEmpty() ? p.name : "(unnamed)") << "</td>"
                         << "<td class=\"what\">" << escape (summarise (p)) << "</td></tr>\n";
                }
            }
            if (rows.isEmpty()) h << "<p class=\"none\">No programs yet.</p>\n";
            else                h << "<table>\n" << rows << "</table>\n";
        }
        h << "</body></html>\n";
        return h;
    }
};

} // namespace perf
