#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <array>
#include <functional>
#include <vector>

// Drawing kit for the flat panel: plain background, line-only knobs, floating bead lettering
// and dot text with a small extrusion. No outlines anywhere.
namespace ui
{
using juce::Colour;

struct Theme
{
    Colour bg, ink, sub, hi, extrude, line, off, wave, accentText;
    std::array<Colour, 4> bead;
    bool dark = false;
};

inline Colour hex (juce::uint32 rgb) { return Colour (0xff000000u | rgb); }

inline Theme themeFor (int idx)
{
    Theme t;
    if (idx == 1)
    {
        t.bg = hex (0xC9C9CF); t.ink = hex (0x1E1E24); t.sub = hex (0x33333B); t.hi = Colour (0x73ffffff); t.extrude = hex (0x9A9AA2);
        t.line = hex (0x1E1E24); t.off = hex (0xA6A6AE); t.wave = hex (0x244F92); t.accentText = hex (0x7E340B);
        t.bead = { hex (0xFFFFFF), hex (0xE2E2E8), hex (0xA9A9B3), hex (0x85858F) };
    }
    else if (idx == 2)
    {
        t.dark = true;
        t.bg = hex (0x1E1E23); t.ink = hex (0xEDEDF3); t.sub = hex (0xB9B9C5); t.hi = Colour (0xff000000); t.extrude = hex (0x0A0A0D);
        t.line = hex (0xEDEDF3); t.off = hex (0x3A3A42); t.wave = hex (0x86AEF0); t.accentText = hex (0xF7A162);
        t.bead = { hex (0xFFFFFF), hex (0xD8D8E2), hex (0x8C8C9A), hex (0x5E5E6A) };
    }
    else
    {
        t.bg = hex (0xF7F7F9); t.ink = hex (0x3D3A4A); t.sub = hex (0x625D7A); t.hi = Colour (0xffffffff); t.extrude = hex (0xCFCCD9);
        t.line = hex (0x3D3A4A); t.off = hex (0xD9D7E1); t.wave = hex (0x3467B0); t.accentText = hex (0xA84C14);
        t.bead = { hex (0xFFFFFF), hex (0xE9E7F1), hex (0xB9B3CD), hex (0x9C96B5) };
    }
    return t;
}

inline const Colour accent = hex (0xE8742A);

juce::Font dotFont (float height);

// Dot text with a pushed-out look.
inline void text3D (juce::Graphics& g, const Theme& t, const juce::String& s, juce::Rectangle<float> r,
                    juce::Justification j, float size, Colour col, bool extrude = true)
{
    g.setFont (dotFont (size));
    if (extrude)
    {
        g.setColour (juce::Colours::black.withAlpha (t.dark ? 0.5f : 0.14f));
        g.drawText (s, r.translated (0, 3.5f), j, false);
        g.setColour (t.extrude);
        g.drawText (s, r.translated (0, 2.0f), j, false);
        g.setColour (t.hi.withMultipliedAlpha (0.9f));
        g.drawText (s, r.translated (0, 1.0f), j, false);
    }
    g.setColour (col);
    g.drawText (s, r, j, false);
}

// 5x7 bead lettering, floating above the panel.
inline const char* const* glyph (juce::juce_wchar c)
{
    static const char* A[] { " ### ", "#   #", "#   #", "#####", "#   #", "#   #", "#   #" };
    static const char* B[] { "#### ", "#   #", "#   #", "#### ", "#   #", "#   #", "#### " };
    static const char* C[] { " ####", "#    ", "#    ", "#    ", "#    ", "#    ", " ####" };
    static const char* Q[] { " ### ", "#   #", "#   #", "#   #", "# # #", "#  # ", " ## #" };
    static const char* D[] { "#### ", "#   #", "#   #", "#   #", "#   #", "#   #", "#### " };
    static const char* E[] { "#####", "#    ", "#    ", "#### ", "#    ", "#    ", "#####" };
    static const char* F[] { "#####", "#    ", "#    ", "#### ", "#    ", "#    ", "#    " };
    static const char* G[] { " ####", "#    ", "#    ", "#  ##", "#   #", "#   #", " ### " };
    static const char* H[] { "#   #", "#   #", "#   #", "#####", "#   #", "#   #", "#   #" };
    static const char* I[] { " ### ", "  #  ", "  #  ", "  #  ", "  #  ", "  #  ", " ### " };
    static const char* K[] { "#   #", "#  # ", "# #  ", "##   ", "# #  ", "#  # ", "#   #" };
    static const char* L[] { "#    ", "#    ", "#    ", "#    ", "#    ", "#    ", "#####" };
    static const char* M[] { "#   #", "## ##", "# # #", "# # #", "#   #", "#   #", "#   #" };
    static const char* N[] { "#   #", "##  #", "# # #", "#  ##", "#   #", "#   #", "#   #" };
    static const char* O[] { " ### ", "#   #", "#   #", "#   #", "#   #", "#   #", " ### " };
    static const char* P[] { "#### ", "#   #", "#   #", "#### ", "#    ", "#    ", "#    " };
    static const char* R[] { "#### ", "#   #", "#   #", "#### ", "# #  ", "#  # ", "#   #" };
    static const char* S[] { " ####", "#    ", "#    ", " ### ", "    #", "    #", "#### " };
    static const char* T[] { "#####", "  #  ", "  #  ", "  #  ", "  #  ", "  #  ", "  #  " };
    static const char* U[] { "#   #", "#   #", "#   #", "#   #", "#   #", "#   #", " ### " };
    static const char* V[] { "#   #", "#   #", "#   #", "#   #", "#   #", " # # ", "  #  " };
    static const char* W[] { "#   #", "#   #", "#   #", "# # #", "# # #", "## ##", "#   #" };
    static const char* X[] { "#   #", "#   #", " # # ", "  #  ", " # # ", "#   #", "#   #" };
    static const char* Y[] { "#   #", "#   #", " # # ", "  #  ", "  #  ", "  #  ", "  #  " };
    static const char* n1[] { "  #  ", " ##  ", "  #  ", "  #  ", "  #  ", "  #  ", " ### " };
    static const char* n2[] { " ### ", "#   #", "    #", "   # ", "  #  ", " #   ", "#####" };
    static const char* n3[] { "#####", "   # ", "  #  ", "   # ", "    #", "#   #", " ### " };
    static const char* n4[] { "   # ", "  ## ", " # # ", "#  # ", "#####", "   # ", "   # " };
    static const char* lb[] { " ### ", " #   ", " #   ", " #   ", " #   ", " #   ", " ### " };
    static const char* rb[] { " ### ", "   # ", "   # ", "   # ", "   # ", "   # ", " ### " };
    switch (c)
    {
        case 'A': return A; case 'B': return B; case 'C': return C; case 'D': return D; case 'E': return E;
        case 'F': return F; case 'G': return G; case 'H': return H; case 'I': return I; case 'K': return K;
        case 'L': return L; case 'M': return M; case 'N': return N; case 'O': return O; case 'P': return P; case 'Q': return Q;
        case 'R': return R; case 'S': return S; case 'T': return T; case 'U': return U; case 'V': return V;
        case 'W': return W; case 'X': return X; case 'Y': return Y;
        case '1': return n1; case '2': return n2; case '3': return n3; case '4': return n4;
        case '[': return lb; case ']': return rb;
        default: return nullptr;
    }
}

inline float beadsWidth (const juce::String& s, float pitch)
{
    int col = 0;
    for (auto c : s) col += (c == ' ' || glyph (c) == nullptr) ? 3 : 6;
    return (float) juce::jmax (1, col - 1) * pitch;
}

inline void beads (juce::Graphics& g, const Theme& t, const juce::String& s, float x, float y, float pitch)
{
    struct Dot { float x, y, s; };
    std::vector<Dot> dots;
    int col = 0, k = 0;
    for (auto c : s)
    {
        auto gl = glyph (c);
        if (c == ' ' || gl == nullptr) { col += 3; continue; }
        for (int r = 0; r < 7; ++r)
            for (int cc = 0; cc < 5; ++cc)
            {
                if (gl[r][cc] != '#') continue;
                ++k;
                const float vr = 0.84f + (float) ((k * 9301 + 49297) % 233280) / 233280.0f * 0.24f;
                dots.push_back ({ x + ((float) (col + cc) + 0.5f) * pitch, y + ((float) r + 0.5f) * pitch, pitch * vr });
            }
        col += 6;
    }
    const float off = juce::jmax (3.0f, pitch * 1.1f);
    for (auto& d : dots)
    {
        g.setColour (juce::Colours::black.withAlpha (t.dark ? 0.2f : 0.07f));
        g.fillEllipse (juce::Rectangle<float> (d.s * 1.5f, d.s * 1.5f).withCentre ({ d.x, d.y + off }));
        g.setColour (juce::Colours::black.withAlpha (t.dark ? 0.28f : 0.12f));
        g.fillEllipse (juce::Rectangle<float> (d.s, d.s).withCentre ({ d.x, d.y + off * 0.8f }));
    }
    for (auto& d : dots)
    {
        auto r = juce::Rectangle<float> (d.s, d.s).withCentre ({ d.x, d.y });
        juce::ColourGradient cg (t.bead[0], r.getX() + r.getWidth() * 0.32f, r.getY() + r.getHeight() * 0.28f,
                                 t.bead[3], r.getRight(), r.getBottom(), true);
        cg.addColour (0.18, t.bead[0]);
        cg.addColour (0.45, t.bead[1]);
        cg.addColour (0.85, t.bead[2]);
        g.setGradientFill (cg);
        g.fillEllipse (r);
    }
}
} // namespace ui
