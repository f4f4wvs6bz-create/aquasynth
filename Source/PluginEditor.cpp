#include "PluginEditor.h"
#include "Ui.h"
#include "SerumImport.h"
#include "BinaryData.h"
#include <juce_dsp/juce_dsp.h>

using namespace juce;
using ui::Theme;

juce::Font ui::dotFont (float height)
{
    static Typeface::Ptr tf = Typeface::createSystemTypefaceFor (BinaryData::DotoBlackRound_ttf, BinaryData::DotoBlackRound_ttfSize);
    return Font (FontOptions().withTypeface (tf).withHeight (height * 1.18f));
}

namespace
{
const String dot = String::charToString ((juce_wchar) 0x00B7);

// ============================================================ shared context
struct Ctx
{
    AquaSynthProcessor& proc;
    AudioProcessorValueTreeState& apvts;
    Theme theme;
    int themeIdx = 0, selLfo = 0, envTab = 0; // envTab: 0 filter, 1 amp, 2 pitch
    std::function<void()> onAnyChange;

    explicit Ctx (AquaSynthProcessor& p) : proc (p), apvts (p.apvts) {}
    RangedAudioParameter* param (const String& id) const { return apvts.getParameter (id); }
    float get (const String& id) const { return apvts.getRawParameterValue (id)->load(); }
    float norm (const String& id) const { return param (id)->getValue(); }
    void setNorm (const String& id, float v)
    {
        auto* p = param (id);
        p->beginChangeGesture(); p->setValueNotifyingHost (jlimit (0.0f, 1.0f, v)); p->endChangeGesture();
    }
    void setPlain (const String& id, float v) { setNorm (id, param (id)->convertTo0to1 (v)); }
    String lfo (int l, const char* n) const { return us::lfoId (l, n); }
    String osc (int o, const char* n) const { return us::oscId (o, n); }

    void toggleRoute (int l, int dest)
    {
        std::vector<int> d;
        for (auto* n : { "D1", "D2" }) { int v = (int) get (lfo (l, n)); if (v != us::dNone) d.push_back (v); }
        auto it = std::find (d.begin(), d.end(), dest);
        if (it != d.end()) d.erase (it);
        else if (d.size() < 2) d.push_back (dest);
        else d[1] = dest;
        while (d.size() < 2) d.push_back (us::dNone);
        setPlain (lfo (l, "D1"), (float) d[0]);
        setPlain (lfo (l, "D2"), (float) d[1]);
    }
};

float waveAt (int type, float t, float p, int table)
{
    const float x = t - std::floor (t), a = MathConstants<float>::twoPi * x;
    switch (type)
    {
        case us::tSine: return std::sin (a);
        case us::tTri: return 1.0f - 4.0f * std::abs (x - 0.5f);
        case us::tSaw: return 2.0f * x - 1.0f;
        case us::tPulse: return x < 0.5f ? 1.0f : -1.0f;
        case us::tWave:
            if (table == 0)
            {
                const float s = p * 3.0f;
                const float sh[4] { std::sin (a), 1.0f - 4.0f * std::abs (x - 0.5f), 2.0f * x - 1.0f, x < 0.5f ? 1.0f : -1.0f };
                const int i0 = jmin ((int) s, 2); const float f = s - (float) i0;
                return sh[i0] * (1 - f) + sh[i0 + 1] * f;
            }
            if (table == 1) return (2.0f * x - 1.0f) * (1 - p) + (x < 0.5f - 0.42f * p ? 1.0f : -1.0f) * p;
            return (std::sin (a) + 0.55f * std::sin (3 * a + p * 4) + 0.35f * p * std::sin (5 * a + p * 2) + 0.25f * std::sin (2 * a)) / 1.5f;
        case us::tFm: return std::sin (a + (0.5f + p * 3) * std::sin (2 * a));
        case us::tFormant: return jlimit (-1.0f, 1.0f, std::sin (a * (3.0f + (float) roundToInt (p * 5))) * std::exp (-3 * x) * 1.3f);
        default:
        {
            static const float r[] { .6f, -.4f, .9f, -.8f, .2f, -.1f, .7f, -.6f, .3f, -.9f, .5f, 0.f, -.3f, .8f, -.7f, .4f };
            return r[(int) std::floor (t * 16) & 15];
        }
    }
}

float lfoAt (int shape, float t)
{
    const float x = t - std::floor (t);
    switch (shape)
    {
        case 0: return std::sin (MathConstants<float>::twoPi * x);
        case 1: return 1.0f - 4.0f * std::abs (x - 0.5f);
        case 2: return x < 0.5f ? 1.0f : -1.0f;
        case 3: return 2.0f * x - 1.0f;
        default: { static const float r[] { .6f, -.4f, .9f, -.8f, .2f, -.1f, .7f, -.6f }; return r[(int) std::floor (t * 4) & 7]; }
    }
}

template <typename Fn>
Path tracePath (Fn fn, float x0, float y0, float W, float H, float amp)
{
    Path path;
    for (int i = 0; i <= 200; ++i)
    {
        const float t = (float) i / 200.0f;
        const float y = jlimit (y0 + 2, y0 + H - 2, y0 + H * 0.5f - fn (t) * amp);
        if (i == 0) path.startNewSubPath (x0 + t * W, y); else path.lineTo (x0 + t * W, y);
    }
    return path;
}

// ============================================================ knob: only the pointer line
struct LineKnob : public Slider
{
    Ctx& ctx; int size;
    LineKnob (Ctx& c, int s) : ctx (c), size (s)
    {
        setSliderStyle (Slider::RotaryVerticalDrag);
        setTextBoxStyle (Slider::NoTextBox, false, 0, 0);
        setMouseDragSensitivity (200);
        setScrollWheelEnabled (true);
    }
    void paint (Graphics& g) override
    {
        auto& t = ctx.theme;
        const float s = (float) size;
        const Point<float> c (getWidth() * 0.5f, s * 0.5f + 2.0f);
        const float prop = (float) valueToProportionOfLength (getValue());
        const float ang = degreesToRadians (-135.0f + 270.0f * prop);
        const float w = s >= 50 ? 3.0f : (s >= 30 ? 2.5f : 2.0f), len = s * 0.5f;
        Path n; n.addRoundedRectangle (-w * 0.5f, -len, w, len, w * 0.5f);
        const auto tr = AffineTransform::rotation (ang).translated (c.x, c.y);
        g.setColour (Colours::black.withAlpha (t.dark ? 0.55f : 0.2f));
        g.fillPath (n, tr.translated (0, 2.0f));
        g.setColour (t.line);
        g.fillPath (n, tr);
    }
};

struct KnobUnit : public Component, private Slider::Listener
{
    enum Style { below, macro, mini, lfoBox };
    Ctx& ctx; LineKnob knob; Style style; String label, sub;
    std::unique_ptr<AudioProcessorValueTreeState::SliderAttachment> att;

    KnobUnit (Ctx& c, int size, Style st, String lab, String s = {})
        : ctx (c), knob (c, size), style (st), label (std::move (lab)), sub (std::move (s))
    {
        addAndMakeVisible (knob);
        knob.addListener (this);
    }
    ~KnobUnit() override { knob.removeListener (this); }
    void bind (const String& id, const String& newLabel = {})
    {
        att.reset();
        att = std::make_unique<AudioProcessorValueTreeState::SliderAttachment> (ctx.apvts, id, knob);
        if (newLabel.isNotEmpty()) label = newLabel;
        knob.setDoubleClickReturnValue (true, ctx.param (id)->convertFrom0to1 (ctx.param (id)->getDefaultValue()));
        knob.setTitle (ctx.param (id)->getName (40));
        repaint();
    }
    void setSub (const String& s) { if (s != sub) { sub = s; repaint(); } }
    void sliderValueChanged (Slider*) override { repaint(); }
    String valueText() { return knob.getTextFromValue (knob.getValue()); }

    void resized() override
    {
        const int s = knob.size + 4;
        if (style == mini) knob.setBounds (0, (getHeight() - s) / 2, s, s);
        else if (style == lfoBox) knob.setBounds ((getWidth() - s) / 2, 16, s, s);
        else knob.setBounds ((getWidth() - s) / 2, 0, s, s);
    }
    void paint (Graphics& g) override
    {
        auto& t = ctx.theme;
        const float w = (float) getWidth();
        const float kBottom = (float) knob.getBottom();
        const float lab = knob.size >= 50 ? 14.0f : (knob.size >= 36 ? 12.0f : 11.0f);
        if (style == mini)
        {
            const float x = (float) knob.getRight() + 2.0f;
            ui::text3D (g, t, label, { x, 2, w - x, 11 }, Justification::centredLeft, 9.0f, t.ink, false);
            ui::text3D (g, t, valueText(), { x, 13, w - x, 12 }, Justification::centredLeft, 10.0f, t.sub, false);
            return;
        }
        if (style == lfoBox)
        {
            ui::text3D (g, t, label, { 0, 0, w, 14 }, Justification::centred, lab, t.ink);
            ui::text3D (g, t, valueText(), { 0, kBottom + 2, w, 14 }, Justification::centred, 11.0f, t.ink, false);
            ui::text3D (g, t, sub, { 0, kBottom + 17, w, 12 }, Justification::centred, 9.0f, t.sub, false);
            return;
        }
        ui::text3D (g, t, label, { 0, kBottom + 2, w, 15 }, Justification::centred, lab, t.ink);
        auto second = style == macro ? sub + " " + dot + " " + valueText() : valueText();
        ui::text3D (g, t, second, { 0, kBottom + 18, w, 13 }, Justification::centred, 11.0f, t.sub, false);
    }
};

// ============================================================ text button (no frame)
struct TextBtn : public Component
{
    Ctx& ctx; String text; bool on = false; float alpha = 1.0f, fontSize = 12.0f; Justification just = Justification::centredLeft;
    std::function<void()> onClick;
    TextBtn (Ctx& c, String t) : ctx (c), text (std::move (t)) { setMouseCursor (MouseCursor::PointingHandCursor); }
    void set (const String& t, bool isOn, float a = 1.0f)
    {
        if (t != text || isOn != on || a != alpha) { text = t; on = isOn; alpha = a; repaint(); }
    }
    void paint (Graphics& g) override
    {
        auto& th = ctx.theme;
        g.beginTransparencyLayer (alpha);
        ui::text3D (g, th, text, getLocalBounds().toFloat().withTrimmedBottom (3), just, fontSize, on ? th.accentText : th.ink);
        g.endTransparencyLayer();
    }
    void mouseUp (const MouseEvent& e) override { if (getLocalBounds().contains (e.getPosition()) && onClick) onClick(); }
};

// round colour dot (theme picker)
struct Swatch : public Component
{
    Ctx& ctx; Colour col; bool on = false; std::function<void()> onClick;
    Swatch (Ctx& c, Colour cl) : ctx (c), col (cl) { setMouseCursor (MouseCursor::PointingHandCursor); }
    void paint (Graphics& g) override
    {
        auto r = getLocalBounds().toFloat().reduced (3).withTrimmedBottom (2);
        g.setColour (Colours::black.withAlpha (0.2f)); g.fillEllipse (r.translated (0, 2.5f));
        if (on) { g.setColour (ui::accent); g.fillEllipse (r.expanded (2.0f)); }
        g.setColour (col); g.fillEllipse (r);
    }
    void mouseUp (const MouseEvent& e) override { if (getLocalBounds().contains (e.getPosition()) && onClick) onClick(); }
};

// ============================================================ page navigation
struct Nav : public Component
{
    Ctx& ctx; std::function<void (int)> onPage; int page = 0;
    TextBtn b[3] { { ctx, "MAIN" }, { ctx, "FX" }, { ctx, "PRESETS" } };
    explicit Nav (Ctx& c) : ctx (c)
    {
        int x = 0;
        const int w[3] { 52, 32, 78 };
        for (int i = 0; i < 3; ++i)
        {
            addAndMakeVisible (b[i]); b[i].fontSize = 13.0f; b[i].setBounds (x, 0, w[i], 26); x += w[i] + 12;
            b[i].onClick = [this, i] { if (onPage) onPage (i); };
        }
        setSize (x, 26);
    }
    void setPage (int p) { page = p; for (int i = 0; i < 3; ++i) b[i].set (b[i].text, i == p); }
};

// ============================================================ MAIN page pieces
struct MainStatic : public Component
{
    Ctx& ctx;
    explicit MainStatic (Ctx& c) : ctx (c) { setInterceptsMouseClicks (false, false); setBufferedToImage (true); }
    void paint (Graphics& g) override
    {
        auto& t = ctx.theme;
        g.fillAll (t.bg);
        ui::beads (g, t, "AQUASYNTH", 34, 34, 4.6f);
        {
            Graphics::ScopedSaveState ss (g);
            Rectangle<float> st (296, 32, 196, 40);
            g.addTransform (AffineTransform::rotation (degreesToRadians (-2.0f), st.getCentreX(), st.getCentreY()));
            Path sp; sp.addRoundedRectangle (st, 7);
            DropShadow (Colours::black.withAlpha (0.16f), 10, { 0, 5 }).drawForPath (g, sp);
            ColourGradient holo (ui::hex (0xE6E2F6), st.getX(), st.getY(), ui::hex (0xF3DDF0), st.getRight(), st.getBottom(), false);
            holo.addColour (0.18, ui::hex (0xCBE7F4)); holo.addColour (0.36, ui::hex (0xF6D6EA));
            holo.addColour (0.52, ui::hex (0xFFF3C6)); holo.addColour (0.68, ui::hex (0xCFF0E1)); holo.addColour (0.84, ui::hex (0xDCD3F5));
            g.setGradientFill (holo); g.fillPath (sp);
            {
                Graphics::ScopedSaveState s2 (g);
                g.reduceClipRegion (sp);
                ColourGradient shine (Colours::white.withAlpha (0.0f), st.getX() + 10, 0, Colours::white.withAlpha (0.0f), st.getX() + 70, 0, false);
                shine.addColour (0.5, Colours::white.withAlpha (0.7f));
                g.setGradientFill (shine);
                g.fillRect (st.getX() + 10, st.getY() - 10, 60.0f, st.getHeight() + 20);
            }
            g.setColour (ui::hex (0x2E2C38));
            g.setFont (ui::dotFont (13)); g.drawText ("AQUASYNTH", st.withTrimmedLeft (10).withHeight (21).translated (0, 2), Justification::centredLeft, false);
            g.setFont (ui::dotFont (10)); g.drawText ("UPPER-LAYER SYNTH " + dot + " V0.2", st.withTrimmedLeft (10).withTrimmedTop (21), Justification::centredLeft, false);
            ColourGradient pastel (ui::hex (0xF7A8D8), st.getRight() - 24, st.getCentreY() - 8, ui::hex (0xA8F2D2), st.getRight() - 8, st.getCentreY() + 8, false);
            pastel.addColour (0.5, ui::hex (0xA8C8F7));
            g.setGradientFill (pastel);
            g.fillEllipse (Rectangle<float> (16, 16).withCentre ({ st.getRight() - 16, st.getCentreY() }));
        }
        ui::beads (g, t, "MOD ROUTING", 34, 88, 2.6f);
        const String envTitle = ctx.envTab == 1 ? "AMP ENVELOPE" : (ctx.envTab == 2 ? "PITCH ENVELOPE" : "FILTER ENVELOPE");
        ui::beads (g, t, envTitle, 1086.0f - ui::beadsWidth (envTitle, 3.0f), 30, 3.0f);
        ui::text3D (g, t, "OSCILLATORS", { 34, 366, 110, 22 }, Justification::centredLeft, 11.0f, t.sub, false);
        // spread labels
        ui::text3D (g, t, "spread", { 360, 420, 100, 16 }, Justification::centredLeft, 12.0f, t.ink);
        ui::text3D (g, t, "near " + dot + " far", { 640, 420, 100, 16 }, Justification::centredRight, 10.0f, t.sub, false);
        ui::text3D (g, t, "10k", { 360, 444, 28, 10 }, Justification::centredRight, 9.0f, t.sub, false);
        ui::text3D (g, t, "1k", { 360, 559, 28, 10 }, Justification::centredRight, 9.0f, t.sub, false);
        ui::text3D (g, t, "100", { 360, 674, 28, 10 }, Justification::centredRight, 9.0f, t.sub, false);
        ui::text3D (g, t, "L", { 394, 690, 20, 10 }, Justification::centredLeft, 9.0f, t.sub, false);
        ui::text3D (g, t, "C", { 554, 690, 20, 10 }, Justification::centred, 9.0f, t.sub, false);
        ui::text3D (g, t, "R", { 714, 690, 20, 10 }, Justification::centredRight, 9.0f, t.sub, false);
    }
};

struct Routing : public Component
{
    Ctx& ctx;
    const int order[10] { us::dOsc1, us::dOsc2, us::dOsc3, us::dOsc4, us::dPitch, us::dCutoff, us::dReso, us::dPan, us::dWidth, us::dRev };
    const char* names[10] { "OSC1", "OSC2", "OSC3", "OSC4", "PIT", "CUT", "RES", "PAN", "WID", "REV" };
    explicit Routing (Ctx& c) : ctx (c) { setMouseCursor (MouseCursor::PointingHandCursor); }
    bool connected (int l, int dest) const { return (int) ctx.get (ctx.lfo (l, "D1")) == dest || (int) ctx.get (ctx.lfo (l, "D2")) == dest; }
    void paint (Graphics& g) override
    {
        auto& t = ctx.theme;
        for (int m = 0; m < 10; ++m)
            ui::text3D (g, t, names[m], { 30.0f + m * 38.0f, 0, 38, 14 }, Justification::centred, 10.0f, t.sub, false);
        for (int l = 0; l < 3; ++l)
        {
            const float y = 16.0f + l * 28.0f;
            ui::text3D (g, t, "L" + String (l + 1), { 0, y, 28, 28 }, Justification::centredLeft, 11.0f, l == ctx.selLfo ? t.accentText : t.sub, false);
            for (int m = 0; m < 10; ++m)
            {
                const bool on = connected (l, order[m]);
                const Point<float> c (30.0f + m * 38.0f + 19.0f, y + 14.0f);
                if (on)
                {
                    g.setColour (Colours::black.withAlpha (t.dark ? 0.5f : 0.2f));
                    g.fillEllipse (Rectangle<float> (13, 13).withCentre (c.translated (0, 3)));
                    g.setColour (ui::accent);
                    g.fillEllipse (Rectangle<float> (13, 13).withCentre (c));
                }
                else { g.setColour (t.off); g.fillEllipse (Rectangle<float> (8, 8).withCentre (c)); }
            }
        }
    }
    void mouseDown (const MouseEvent& e) override
    {
        const int m = (e.x - 30) / 38, l = (e.y - 16) / 28;
        if (e.x < 30 || m < 0 || m >= 10 || e.y < 16 || l < 0 || l >= 3) return;
        ctx.toggleRoute (l, order[m]);
        repaint();
    }
};

struct EnvCurve : public Component
{
    Ctx& ctx;
    explicit EnvCurve (Ctx& c) : ctx (c) { setInterceptsMouseClicks (false, false); }
    void paint (Graphics& g) override
    {
        auto& t = ctx.theme;
        const float bot = 76, top = 8, ox = 70;
        Path p;
        float c1x, c1y, c2x, c2y;
        if (ctx.envTab == 2)
        {
            const float amt = ctx.get ("pAmt") / 24.0f, d = ctx.norm ("pD");
            const float mid = 42;
            const float startY = mid - amt * 34;
            p.startNewSubPath (ox, startY);
            const float len = 20 + d * 250;
            for (int i = 1; i <= 60; ++i)
            {
                const float x = (float) i / 60.0f;
                p.lineTo (ox + x * len, mid + (startY - mid) * std::exp (-x * 5));
            }
            p.lineTo (ox + 330, mid);
            c1x = ox; c1y = startY; c2x = ox + len; c2y = mid;
        }
        else
        {
            const bool amp = ctx.envTab == 1;
            const float a = ctx.norm (amp ? "aA" : "fA"), d = ctx.norm (amp ? "aD" : "fD"), s = ctx.norm (amp ? "aS" : "fS"),
                        r = ctx.norm (amp ? "aR" : "fR"), amt = ctx.norm (amp ? "aLvl" : "fAmt");
            const float pk = bot - (bot - top) * (0.3f + 0.7f * amt);
            const float sl = bot - (bot - pk) * s;
            const float x0 = ox, x1 = x0 + 10 + a * 70, x2 = x1 + 10 + d * 70, x3 = x2 + 70, x4 = x3 + 10 + r * 70;
            p.startNewSubPath (x0, bot); p.lineTo (x1, pk); p.lineTo (x2, sl); p.lineTo (x3, sl); p.lineTo (x4, bot);
            c1x = x1; c1y = pk; c2x = x3; c2y = sl;
        }
        g.setColour (Colours::black.withAlpha (t.dark ? 0.4f : 0.1f));
        g.strokePath (p, PathStrokeType (3.0f, PathStrokeType::curved, PathStrokeType::rounded), AffineTransform::translation (0, 3));
        g.setColour (t.wave);
        g.strokePath (p, PathStrokeType (3.0f, PathStrokeType::curved, PathStrokeType::rounded));
        g.setColour (ui::accent);
        g.fillEllipse (Rectangle<float> (9, 9).withCentre ({ c1x, c1y }));
        g.fillEllipse (Rectangle<float> (9, 9).withCentre ({ c2x, c2y }));
    }
};

// one oscillator: title, type, LEVEL / TUNE, and either the waveform or its detail knobs
struct OscBlock : public Component
{
    Ctx& ctx; int index; bool details = false; int waveH = 74;
    KnobUnit lvl, tune, oct, fine, uni, det, pos;
    TextBtn table, key;
    std::function<void()> onLayout;
    OscBlock (Ctx& c, int i)
        : ctx (c), index (i), lvl (c, 24, KnobUnit::mini, "LEVEL"), tune (c, 24, KnobUnit::mini, "TUNE"),
          oct (c, 24, KnobUnit::mini, "OCT"), fine (c, 24, KnobUnit::mini, "FINE"), uni (c, 24, KnobUnit::mini, "UNISON"),
          det (c, 24, KnobUnit::mini, "DETUNE"), pos (c, 24, KnobUnit::mini, "POS"), table (c, "TABLE"), key (c, "KEY")
    {
        for (auto* k : { &lvl, &tune, &oct, &fine, &uni, &det, &pos }) addChildComponent (*k);
        lvl.bind (c.osc (i, "Lvl")); tune.bind (c.osc (i, "Semi")); oct.bind (c.osc (i, "Oct")); fine.bind (c.osc (i, "Fine"));
        uni.bind (c.osc (i, "Uni")); det.bind (c.osc (i, "Det")); pos.bind (c.osc (i, "Pos"));
        lvl.setVisible (true); tune.setVisible (true);
        addChildComponent (table); addChildComponent (key);
        table.fontSize = key.fontSize = 10.0f;
        table.onClick = [this] { ctx.setPlain (ctx.osc (index, "Table"), (float) (((int) ctx.get (ctx.osc (index, "Table")) + 1) % 3)); refresh(); };
        key.onClick = [this] { ctx.setPlain (ctx.osc (index, "Key"), ctx.get (ctx.osc (index, "Key")) > 0.5f ? 0.0f : 1.0f); refresh(); };
    }
    int type() const { return (int) ctx.get (ctx.osc (index, "Type")); }
    void setWaveHeight (int h) { waveH = h; resized(); }
    void refresh()
    {
        const int ty = type();
        table.set ("TABLE " + us::wtTables[(int) ctx.get (ctx.osc (index, "Table"))], false);
        key.set (ctx.get (ctx.osc (index, "Key")) > 0.5f ? "KEY ON" : "KEY OFF", false);
        table.setVisible (details && ty == us::tWave);
        key.setVisible (details && ty == us::tSample);
        repaint();
    }
    void resized() override
    {
        lvl.setBounds (getWidth() - 142, 0, 70, 28);
        tune.setBounds (getWidth() - 70, 0, 70, 28);
        const int y = 30;
        KnobUnit* row[5] { &oct, &fine, &uni, &det, &pos };
        for (int k = 0; k < 5; ++k) { row[k]->setBounds (k * 60, y, 60, 32); row[k]->setVisible (details); }
        table.setBounds (0, y + 34, 130, 18); key.setBounds (0, y + 34, 130, 18);
        refresh();
    }
    void paint (Graphics& g) override
    {
        auto& t = ctx.theme;
        ui::beads (g, t, "OSC" + String (index + 1), 0, 5, 2.6f);
        const int ty = type();
        String label = us::oscTypes[ty];
        if (ty == us::tWave) label = "WT " + us::wtTables[(int) ctx.get (ctx.osc (index, "Table"))];
        if (ty == us::tSample)
            label = ctx.proc.getOscSample (index) ? ctx.proc.getOscSample (index)->name.substring (0, 12).toUpperCase() : String ("NO SAMPLE");
        ui::text3D (g, t, details ? "-" : "+", { 62, 2, 12, 24 }, Justification::centred, 12.0f, t.sub, false);
        ui::text3D (g, t, label, { 78, 0, (float) getWidth() - 222, 28 }, Justification::centredLeft, 10.0f, t.sub, false);
        if (details) return;
        const float amp = (waveH * 0.5f - 4) * (0.35f + 0.65f * ctx.get (ctx.osc (index, "Lvl")));
        const float p = ctx.get (ctx.osc (index, "Pos"));
        const int tb = (int) ctx.get (ctx.osc (index, "Table"));
        auto path = tracePath ([&] (float x) { return waveAt (ty, x * 2.0f, p, tb); }, 2, 30, 296, (float) waveH, amp);
        g.setColour (Colours::black.withAlpha (t.dark ? 0.4f : 0.1f));
        g.strokePath (path, PathStrokeType (3.5f, PathStrokeType::curved, PathStrokeType::rounded), AffineTransform::translation (0, 3));
        g.setColour (t.wave);
        g.strokePath (path, PathStrokeType (3.5f, PathStrokeType::curved, PathStrokeType::rounded));
    }
    void mouseDown (const MouseEvent& e) override
    {
        if (e.y < 30 && e.x < 76) { details = ! details; resized(); repaint(); return; }
        if (e.y >= 30 && ! details) ctx.setPlain (ctx.osc (index, "Type"), (float) ((type() + 1) % us::numOscTypes));
        refresh();
    }
};

struct SpreadView : public Component, private Timer
{
    Ctx& ctx;
    dsp::FFT fft { 11 };
    dsp::WindowingFunction<float> window { 2048, dsp::WindowingFunction<float>::hann };
    std::array<float, 4096> fl {}, fr {};
    struct Pt { float pan, wet, y, a; };
    std::vector<Pt> pts;
    explicit SpreadView (Ctx& c) : ctx (c) { setInterceptsMouseClicks (false, false); startTimerHz (24); }
    void timerCallback() override
    {
        if (auto* pg = getParentComponent()) if (! pg->isVisible()) return;
        auto& p = ctx.proc;
        const int w = p.visWrite.load (std::memory_order_relaxed);
        for (int i = 0; i < 2048; ++i)
        {
            const int idx = (w - 2048 + i) & (AquaSynthProcessor::visSize - 1);
            fl[(size_t) i] = p.visL[(size_t) idx]; fr[(size_t) i] = p.visR[(size_t) idx];
        }
        window.multiplyWithWindowingTable (fl.data(), 2048);
        window.multiplyWithWindowingTable (fr.data(), 2048);
        fft.performRealOnlyForwardTransform (fl.data(), true);
        fft.performRealOnlyForwardTransform (fr.data(), true);
        const double sr = jmax (8000.0, p.getSampleRate());
        pts.clear();
        float maxMag = 1.0e-9f;
        for (int k = 2; k < 1024; ++k) maxMag = jmax (maxMag, std::hypot (fl[(size_t) (2 * k)], fl[(size_t) (2 * k + 1)]) + std::hypot (fr[(size_t) (2 * k)], fr[(size_t) (2 * k + 1)]));
        if (maxMag > 0.01f)
            for (int b = 0; b < 150; ++b)
            {
                const float f = 80.0f * std::pow (12000.0f / 80.0f, (float) b / 149.0f);
                const int k = jlimit (2, 1020, (int) std::round (f / sr * 2048.0));
                float ml = 0, mr = 0, cross = 0;
                for (int j = k - 1; j <= k + 1; ++j)
                {
                    const float lr = fl[(size_t) (2 * j)], li = fl[(size_t) (2 * j + 1)], rr = fr[(size_t) (2 * j)], ri = fr[(size_t) (2 * j + 1)];
                    ml += lr * lr + li * li; mr += rr * rr + ri * ri; cross += lr * rr + li * ri;
                }
                const float mag = std::sqrt (ml) + std::sqrt (mr);
                const float db = Decibels::gainToDecibels (mag / maxMag + 1.0e-9f);
                if (db < -50.0f) continue;
                const float coh = cross / (std::sqrt (ml * mr) + 1.0e-12f);
                pts.push_back ({ (std::sqrt (mr) - std::sqrt (ml)) / (std::sqrt (mr) + std::sqrt (ml) + 1.0e-9f),
                                 jlimit (0.0f, 1.0f, (1.0f - coh) * 0.5f), (std::log10 (f) - 2.0f) / 2.0f, jmap (db, -50.0f, 0.0f, 0.3f, 1.0f) });
            }
        repaint();
    }
    void paint (Graphics& g) override
    {
        auto& t = ctx.theme;
        if (pts.empty())
        {
            ui::text3D (g, t, "play a note", getLocalBounds().toFloat(), Justification::centred, 10.0f, t.sub.withAlpha (0.6f), false);
            return;
        }
        const float W = (float) getWidth(), H = (float) getHeight();
        int k = 0;
        for (auto& p : pts)
            for (int j = 0; j < 3; ++j)
            {
                ++k;
                const float jx = (float) ((k * 7919) % 97) / 97.0f - 0.5f, jy = (float) ((k * 104729) % 89) / 89.0f - 0.5f;
                const float far = jlimit (0.0f, 1.0f, p.wet * 1.6f + jx * 0.1f);
                const float size = 5.5f - far * 3.5f;
                const float x = W * 0.5f + jlimit (-1.0f, 1.0f, p.pan + jx * 0.14f) * (W * 0.5f - 4);
                const float y = H - 4 - jlimit (0.0f, 1.0f, p.y) * (H - 8) + jy * 4.0f;
                g.setColour (t.wave.withAlpha (p.a * (1.0f - far * 0.65f)));
                g.fillEllipse (x - size * 0.5f, y - size * 0.5f, size, size);
            }
    }
};

struct LfoRow : public Component
{
    Ctx& ctx; int index; std::function<void()> onSelect;
    LfoRow (Ctx& c, int i) : ctx (c), index (i) { setMouseCursor (MouseCursor::PointingHandCursor); }
    void paint (Graphics& g) override
    {
        auto& t = ctx.theme;
        const bool sel = ctx.selLfo == index;
        const int shape = (int) ctx.get (ctx.lfo (index, "Shape"));
        ui::beads (g, t, "LFO" + String (index + 1), 0, 3, 2.6f);
        ui::text3D (g, t, us::lfoShapes[shape], { 76, 0, 70, 24 }, Justification::centredLeft, 10.0f, t.sub, false);
        const String d1 = us::lfoDests[(int) ctx.get (ctx.lfo (index, "D1"))], d2 = us::lfoDests[(int) ctx.get (ctx.lfo (index, "D2"))];
        ui::text3D (g, t, d1, { 200, 0, 60, 24 }, Justification::centredRight, 10.0f, t.ink, false);
        ui::text3D (g, t, d2, { 262, 0, 58, 24 }, Justification::centredRight, 10.0f, t.ink, false);
        const float depth = ctx.get (ctx.lfo (index, "Depth"));
        const bool sync = ctx.get (ctx.lfo (index, "Sync")) > 0.5f;
        const float rateN = ctx.norm (ctx.lfo (index, sync ? "Div" : "Hz"));
        const float cycles = 1.0f + (float) roundToInt (rateN * 3.0f);
        auto path = tracePath ([&] (float x) { return lfoAt (shape, x * cycles); }, 2, 28, 316, 60, 6 + depth * 22);
        g.setColour (Colours::black.withAlpha (t.dark ? 0.4f : 0.1f));
        g.strokePath (path, PathStrokeType (sel ? 3.0f : 2.2f, PathStrokeType::curved, PathStrokeType::rounded), AffineTransform::translation (0, 3));
        g.setColour (ui::accent.withAlpha (sel ? 1.0f : 0.5f));
        g.strokePath (path, PathStrokeType (sel ? 3.0f : 2.2f, PathStrokeType::curved, PathStrokeType::rounded));
    }
    void mouseDown (const MouseEvent& e) override
    {
        if (e.y < 26 && e.x >= 76 && e.x < 150) ctx.setPlain (ctx.lfo (index, "Shape"), (float) (((int) ctx.get (ctx.lfo (index, "Shape")) + 1) % 5));
        else if (e.y < 26 && e.x >= 200)
        {
            const auto id = ctx.lfo (index, e.x < 261 ? "D1" : "D2");
            ctx.setPlain (id, (float) (((int) ctx.get (id) + 1) % us::numDests));
        }
        else { ctx.selLfo = index; if (onSelect) onSelect(); }
        repaint();
    }
};

struct LfoHeader : public Component
{
    Ctx& ctx;
    explicit LfoHeader (Ctx& c) : ctx (c) { setInterceptsMouseClicks (false, false); }
    void paint (Graphics& g) override
    {
        auto& t = ctx.theme;
        ui::text3D (g, t, "EDIT " + dot + " LFO" + String (ctx.selLfo + 1), { 0, 0, (float) getWidth(), 16 }, Justification::centredRight, 11.0f, t.accentText, false);
        const String s = (ctx.proc.hostSynced.load() ? "HOST " : "TEMPO ") + String (roundToInt (ctx.proc.hostBpm.load())) + " BPM";
        ui::text3D (g, t, s, { 0, 54, (float) getWidth(), 14 }, Justification::centredRight, 10.0f, t.sub, false);
    }
};

struct RandomBtn : public Component
{
    Ctx& ctx; std::function<void()> onClick; bool down = false;
    explicit RandomBtn (Ctx& c) : ctx (c) { setMouseCursor (MouseCursor::PointingHandCursor); }
    void paint (Graphics& g) override { ui::beads (g, ctx.theme, "RANDOM", 4, down ? 7.0f : 4.0f, 4.0f); }
    void mouseDown (const MouseEvent&) override { down = true; repaint(); }
    void mouseUp (const MouseEvent& e) override { down = false; repaint(); if (getLocalBounds().contains (e.getPosition()) && onClick) onClick(); }
};

// ============================================================ MAIN page
struct MainPage : public Component
{
    Ctx& ctx;
    MainStatic bg { ctx };
    Nav nav { ctx };
    Swatch sw[3] { { ctx, ui::hex (0xFFFFFF) }, { ctx, ui::hex (0xB9B9C0) }, { ctx, ui::hex (0x26262C) } };
    Routing routing { ctx };
    TextBtn arp { ctx, "ARP" }, oct { ctx, "OCT" }, voice { ctx, "POLY" };
    TextBtn tabs[3] { { ctx, "FILTER" }, { ctx, "AMP" }, { ctx, "PITCH" } };
    EnvCurve env { ctx };
    std::unique_ptr<KnobUnit> envKnobs[5], macros[6], lfoKnobs[3];
    TextBtn count[4] { { ctx, "1" }, { ctx, "2" }, { ctx, "3" }, { ctx, "4" } };
    std::unique_ptr<OscBlock> oscs[us::numOscs];
    RandomBtn random { ctx };
    TextBtn target { ctx, "TARGET" }, little { ctx, "A LITTLE" }, full { ctx, "FULL" }, undo { ctx, "UNDO" };
    SpreadView spread { ctx };
    LfoHeader lfoHeader { ctx };
    TextBtn syncB { ctx, "SYNC" }, hzB { ctx, "HZ" };
    std::unique_ptr<LfoRow> lfoRows[3];
    std::function<void (int)> setTheme;

    int randTarget = 0; bool randFull = true;
    std::vector<std::vector<float>> history;
    int boundLfo = -1, boundSync = -1, boundTab = -1, laidCount = -1;

    explicit MainPage (Ctx& c) : ctx (c)
    {
        addAndMakeVisible (bg); bg.setBounds (0, 0, 1120, 840);
        addAndMakeVisible (nav); nav.setTopLeftPosition (466, 40); nav.setPage (0);
        for (int i = 0; i < 3; ++i) { addAndMakeVisible (sw[i]); sw[i].setBounds (552 + i * 28, 38, 28, 28); sw[i].setTitle (StringArray { "White body", "Gray body", "Black body" }[i]); sw[i].onClick = [this, i] { if (setTheme) setTheme (i); }; }
        nav.setTopLeftPosition (636 - nav.getWidth(), 84);

        addAndMakeVisible (routing); routing.setBounds (34, 113, 30 + 38 * 10, 100);
        TextBtn* ap[] { &arp, &oct, &voice };
        for (int i = 0; i < 3; ++i) { addAndMakeVisible (*ap[i]); ap[i]->setBounds (470, 118 + i * 30, 150, 24); }
        arp.onClick   = [this] { ctx.setPlain ("arpMode", (float) (((int) ctx.get ("arpMode") + 1) % 5)); };
        oct.onClick   = [this] { ctx.setPlain ("arpOct", (float) ((int) ctx.get ("arpOct") % 3 + 1)); };
        voice.onClick = [this] { ctx.setPlain ("voice", (float) (((int) ctx.get ("voice") + 1) % 3)); };

        int tx = 1086 - (int) ui::beadsWidth ("FILTER ENVELOPE", 3.0f) - 16 - 170;
        const int tw[3] { 58, 40, 52 };
        for (int i = 0; i < 3; ++i)
        {
            addAndMakeVisible (tabs[i]); tabs[i].fontSize = 11.0f; tabs[i].setBounds (tx, 28, tw[i], 22); tx += tw[i] + 10;
            tabs[i].onClick = [this, i] { ctx.envTab = i; refresh(); };
        }
        addAndMakeVisible (env); env.setBounds (666, 58, 420, 86);
        for (int i = 0; i < 5; ++i)
        {
            envKnobs[i] = std::make_unique<KnobUnit> (ctx, 44, KnobUnit::below, "");
            addAndMakeVisible (*envKnobs[i]);
            envKnobs[i]->setBounds (666 + i * 85, 150, 80, 82);
        }

        const char* mIds[6] { "cutoff", "reso", "drift", "width", "digital", "space" };
        const char* mLab[6] { "CUTOFF", "RESO", "DRIFT", "WIDTH", "90S", "SPACE" };
        const char* mSub[6] { "bright", "edge", "wobble", "spread", "digital", "room" };
        for (int i = 0; i < 6; ++i)
        {
            macros[i] = std::make_unique<KnobUnit> (ctx, 60, KnobUnit::macro, mLab[i], mSub[i]);
            addAndMakeVisible (*macros[i]);
            macros[i]->setBounds (74 + i * 186, 252, 120, 100);
            macros[i]->bind (mIds[i]);
        }

        for (int i = 0; i < 4; ++i)
        {
            addAndMakeVisible (count[i]); count[i].fontSize = 13.0f; count[i].setBounds (150 + i * 26, 366, 22, 24); count[i].just = Justification::centred;
            count[i].onClick = [this, i] { ctx.setPlain ("oscCount", (float) (i + 1)); };
        }
        for (int i = 0; i < us::numOscs; ++i) { oscs[i] = std::make_unique<OscBlock> (ctx, i); addChildComponent (*oscs[i]); }

        addAndMakeVisible (random); random.setBounds (360, 366, 150, 40);
        random.onClick = [this] { doRandom(); };
        addAndMakeVisible (target); target.fontSize = 11.0f; target.setBounds (540, 366, 200, 20);
        addAndMakeVisible (little); little.fontSize = 11.0f; little.setBounds (540, 388, 70, 20);
        addAndMakeVisible (full); full.fontSize = 11.0f; full.setBounds (614, 388, 40, 20);
        addAndMakeVisible (undo); undo.fontSize = 11.0f; undo.setBounds (660, 388, 50, 20);
        target.onClick = [this] { randTarget = (randTarget + 1) % 6; refresh(); };
        little.onClick = [this] { randFull = false; refresh(); };
        full.onClick = [this] { randFull = true; refresh(); };
        undo.onClick = [this] { undoRandom(); };

        addAndMakeVisible (spread); spread.setBounds (394, 446, 340, 236);

        const char* lLab[3] { "RATE", "DEPTH", "SMOOTH" };
        for (int i = 0; i < 3; ++i)
        {
            lfoKnobs[i] = std::make_unique<KnobUnit> (ctx, 40, KnobUnit::lfoBox, lLab[i]);
            addAndMakeVisible (*lfoKnobs[i]);
            lfoKnobs[i]->setBounds (766 + i * 66, 366, 66, 106);
        }
        addAndMakeVisible (lfoHeader); lfoHeader.setBounds (966, 366, 120, 72);
        addAndMakeVisible (syncB); addAndMakeVisible (hzB);
        syncB.fontSize = hzB.fontSize = 11.0f;
        syncB.setBounds (1004, 386, 44, 20); hzB.setBounds (1056, 386, 30, 20);
        syncB.onClick = [this] { ctx.setPlain (ctx.lfo (ctx.selLfo, "Sync"), 1.0f); };
        hzB.onClick = [this] { ctx.setPlain (ctx.lfo (ctx.selLfo, "Sync"), 0.0f); };
        for (int i = 0; i < 3; ++i)
        {
            lfoRows[i] = std::make_unique<LfoRow> (ctx, i);
            addAndMakeVisible (*lfoRows[i]);
            lfoRows[i]->setBounds (766, 482 + i * 108, 320, 92);
            lfoRows[i]->onSelect = [this] { refresh(); };
        }
    }

    void layoutOscs()
    {
        const int n = jlimit (1, us::numOscs, (int) ctx.get ("oscCount"));
        if (n == laidCount) return;
        laidCount = n;
        const int h = n >= 4 ? 52 : (n == 3 ? 74 : (n == 2 ? 118 : 220));
        int y = 394;
        for (int i = 0; i < us::numOscs; ++i)
        {
            oscs[i]->setVisible (i < n);
            oscs[i]->setBounds (34, y, 300, 30 + h);
            oscs[i]->setWaveHeight (h);
            if (i < n) y += 30 + h + 6;
        }
    }

    void rebind()
    {
        if (boundTab != ctx.envTab)
        {
            boundTab = ctx.envTab;
            if (ctx.envTab == 2)
            {
                envKnobs[0]->setVisible (false); envKnobs[3]->setVisible (false); envKnobs[4]->setVisible (false);
                envKnobs[1]->bind ("pD", "DECAY"); envKnobs[2]->bind ("pAmt", "AMOUNT");
            }
            else
            {
                const char* f[5] { "fA", "fD", "fAmt", "fS", "fR" };
                const char* a[5] { "aA", "aD", "aLvl", "aS", "aR" };
                const char* lab[5] { "ATTACK", "DECAY", ctx.envTab == 1 ? "LEVEL" : "AMOUNT", "SUSTAIN", "RELEASE" };
                for (int i = 0; i < 5; ++i) { envKnobs[i]->setVisible (true); envKnobs[i]->bind (ctx.envTab == 1 ? a[i] : f[i], lab[i]); }
            }
            bg.repaint();
        }
        const int sync = ctx.get (ctx.lfo (ctx.selLfo, "Sync")) > 0.5f ? 1 : 0;
        if (boundLfo != ctx.selLfo || boundSync != sync)
        {
            boundLfo = ctx.selLfo; boundSync = sync;
            lfoKnobs[0]->bind (ctx.lfo (ctx.selLfo, sync ? "Div" : "Hz"));
            lfoKnobs[1]->bind (ctx.lfo (ctx.selLfo, "Depth"));
            lfoKnobs[2]->bind (ctx.lfo (ctx.selLfo, "Smooth"));
        }
        if (sync)
        {
            const int div = jlimit (0, 7, (int) ctx.get (ctx.lfo (ctx.selLfo, "Div")));
            const float bpm = ctx.proc.hostBpm.load();
            lfoKnobs[0]->setSub ("= " + String (bpm / 60.0f / us::lfoDivBeats[div], 2) + "Hz");
        }
        else lfoKnobs[0]->setSub ("free");
    }

    void refresh()
    {
        rebind();
        layoutOscs();
        arp.set ("ARP " + us::arpModes[(int) ctx.get ("arpMode")], (int) ctx.get ("arpMode") > 0);
        oct.set ("OCT " + String ((int) ctx.get ("arpOct")), false);
        voice.set (us::voiceModes[(int) ctx.get ("voice")], false);
        for (int i = 0; i < 3; ++i) { tabs[i].set (tabs[i].text, ctx.envTab == i); sw[i].on = ctx.themeIdx == i; sw[i].repaint(); }
        const int n = (int) ctx.get ("oscCount");
        for (int i = 0; i < 4; ++i) count[i].set (String (i + 1), n == i + 1);
        const char* tg[6] { "ALL", "OSC ONLY", "LFO ONLY", "ENV ONLY", "FILTER ONLY", "FX ONLY" };
        target.set ("TARGET " + dot + " " + tg[randTarget], false);
        little.set ("A LITTLE", ! randFull); full.set ("FULL", randFull);
        undo.set ("UNDO", false, history.empty() ? 0.4f : 1.0f);
        const bool sync = ctx.get (ctx.lfo (ctx.selLfo, "Sync")) > 0.5f;
        syncB.set ("SYNC", sync); hzB.set ("HZ", ! sync);
        routing.repaint(); env.repaint(); lfoHeader.repaint();
        for (auto& o : oscs) o->refresh();
        for (auto& r : lfoRows) r->repaint();
    }

    // ---------------- random
    std::vector<float> snapshot() const
    {
        std::vector<float> s;
        for (auto* prm : ctx.proc.getParameters()) s.push_back (prm->getValue());
        return s;
    }
    void undoRandom()
    {
        if (history.empty()) return;
        auto s = history.back(); history.pop_back();
        auto& ps = ctx.proc.getParameters();
        for (int i = 0; i < ps.size() && i < (int) s.size(); ++i) { ps[i]->beginChangeGesture(); ps[i]->setValueNotifyingHost (s[(size_t) i]); ps[i]->endChangeGesture(); }
        refresh();
    }
    void doRandom()
    {
        history.push_back (snapshot());
        if (history.size() > 30) history.erase (history.begin());
        Random& r = Random::getSystemRandom();
        const char* keys[6] { "all", "osc", "lfo", "env", "filter", "fx" };
        const String tgt = keys[randTarget];
        auto in = [&] (const char* g) { return tgt == "all" || tgt == g; };
        auto cont = [&] (const String& id, float lo, float hi)
        {
            const float cur = ctx.norm (id);
            ctx.setNorm (id, randFull ? lo + r.nextFloat() * (hi - lo) : jlimit (0.0f, 1.0f, cur + (r.nextFloat() * 2 - 1) * 0.15f));
        };
        if (in ("osc"))
            for (int o = 0; o < us::numOscs; ++o)
            {
                cont (ctx.osc (o, "Lvl"), o == 0 ? 0.5f : 0.0f, o == 0 ? 1.0f : 0.7f); cont (ctx.osc (o, "Pos"), 0, 1); cont (ctx.osc (o, "Det"), 0, 0.6f);
                if (randFull)
                {
                    const int steps[6] { -12, -5, 0, 0, 7, 12 };
                    if (o > 0) ctx.setPlain (ctx.osc (o, "Semi"), (float) steps[r.nextInt (6)]);
                    ctx.setPlain (ctx.osc (o, "Type"), (float) r.nextInt (us::tNoise + 1));
                    ctx.setPlain (ctx.osc (o, "Uni"), (float) (1 + r.nextInt (3)));
                    ctx.setPlain (ctx.osc (o, "Table"), (float) r.nextInt (3));
                }
            }
        if (in ("env"))
        {
            cont ("fA", 0, 0.45f); cont ("fD", 0.1f, 0.75f); cont ("fS", 0, 1); cont ("fR", 0.1f, 0.7f); cont ("fAmt", 0, 0.9f);
            cont ("aA", 0, 0.5f); cont ("aD", 0.15f, 0.8f); cont ("aS", 0.2f, 1); cont ("aR", 0.15f, 0.75f); cont ("aLvl", 0.6f, 0.9f);
        }
        if (in ("filter")) { cont ("cutoff", 0.25f, 0.85f); cont ("reso", 0, 0.7f); if (randFull) ctx.setPlain ("fType", (float) r.nextInt (4)); }
        if (in ("fx"))
        {
            cont ("drift", 0, 0.8f); cont ("width", 0, 1); cont ("digital", 0, 0.6f); cont ("space", 0, 0.6f);
            cont ("choMix", 0, 0.7f); cont ("revMix", 0, 0.6f); cont ("dlyMix", 0, 0.4f);
        }
        if (in ("lfo"))
            for (int l = 0; l < 3; ++l)
            {
                cont (ctx.lfo (l, "Hz"), 0, 1); cont (ctx.lfo (l, "Div"), 0.3f, 1); cont (ctx.lfo (l, "Depth"), 0, 0.45f); cont (ctx.lfo (l, "Smooth"), 0, 1);
                if (randFull)
                {
                    ctx.setPlain (ctx.lfo (l, "Shape"), (float) r.nextInt (5));
                    ctx.setPlain (ctx.lfo (l, "Sync"), r.nextBool() ? 1.0f : 0.0f);
                    const int a = r.nextInt (10), b = r.nextInt (11);
                    ctx.setPlain (ctx.lfo (l, "D1"), (float) a);
                    ctx.setPlain (ctx.lfo (l, "D2"), (float) (b == a ? us::dNone : b));
                }
            }
        refresh();
    }
};

// ============================================================ FX page
struct FxStatic : public Component
{
    Ctx& ctx;
    explicit FxStatic (Ctx& c) : ctx (c) { setInterceptsMouseClicks (false, false); setBufferedToImage (true); }
    void paint (Graphics& g) override
    {
        auto& t = ctx.theme;
        g.fillAll (t.bg);
        ui::beads (g, t, "EFFECTS", 34, 32, 4.4f);
        ui::text3D (g, t, "signal flows left to right, top to bottom", { 230, 40, 400, 20 }, Justification::centredLeft, 12.0f, t.sub, false);
    }
};

struct FxModule : public Component
{
    Ctx& ctx; int index;
    TextBtn onBtn;
    std::unique_ptr<KnobUnit> knobs[3];
    FxModule (Ctx& c, int i) : ctx (c), index (i), onBtn (c, "OFF")
    {
        addAndMakeVisible (onBtn); onBtn.fontSize = 11.0f; onBtn.just = Justification::centredRight;
        onBtn.onClick = [this] { const String id = String (us::fxList[index].id) + "On"; ctx.setPlain (id, ctx.get (id) > 0.5f ? 0.0f : 1.0f); refresh(); };
        static const char* ids[8][3] { { "Drive", "Tone", "Mix" }, { "Rate", "Depth", "Mix" }, { "Rate", "Fb", "Mix" }, { "Time", "Fb", "Mix" },
                                       { "Size", "Damp", "Mix" }, { "Low", "Mid", "High" }, { "Amt", "Speed", "Mix" }, { "Width", "Mono", "Lpf" } };
        static const char* labels[8][3] { { "DRIVE", "TONE", "MIX" }, { "RATE", "DEPTH", "MIX" }, { "RATE", "FEEDBACK", "MIX" }, { "TIME", "FEEDBACK", "MIX" },
                                          { "SIZE", "DAMP", "MIX" }, { "LOW", "MID", "HIGH" }, { "AMOUNT", "SPEED", "MIX" }, { "WIDTH", "MONO LO", "TOP CUT" } };
        for (int k = 0; k < 3; ++k)
        {
            knobs[k] = std::make_unique<KnobUnit> (c, 48, KnobUnit::below, labels[i][k]);
            addAndMakeVisible (*knobs[k]);
            knobs[k]->bind (String (us::fxList[i].id) + ids[i][k]);
        }
    }
    void resized() override
    {
        onBtn.setBounds (getWidth() - 60, 0, 60, 24);
        for (int k = 0; k < 3; ++k) knobs[k]->setBounds (k * 80, 60, 78, 90);
    }
    void refresh()
    {
        const bool on = ctx.get (String (us::fxList[index].id) + "On") > 0.5f;
        onBtn.set (on ? "ON" : "OFF", on);
        setAlpha (on ? 1.0f : 0.5f);
        onBtn.setAlpha (1.0f);
        repaint();
    }
    void paint (Graphics& g) override
    {
        auto& t = ctx.theme;
        static const char* notes[8] { "warm drive, tone, mix", "juno-style, two taps", "short sweeping echo", "synced to the project tempo",
                                      "room to hall", "low shelf " "\xc2\xb7" " peak " "\xc2\xb7" " high shelf", "glue for the whole sound", "width " "\xc2\xb7" " mono lows " "\xc2\xb7" " top cut" };
        ui::text3D (g, t, "0" + String (index + 1), { 0, 0, 24, 24 }, Justification::centredLeft, 11.0f, t.sub, false);
        ui::text3D (g, t, us::fxList[index].name, { 26, 0, 160, 24 }, Justification::centredLeft, 16.0f, t.ink);
        ui::text3D (g, t, String::fromUTF8 (notes[index]), { 0, 26, (float) getWidth(), 16 }, Justification::centredLeft, 10.0f, t.sub, false);
    }
};

struct FxPage : public Component
{
    Ctx& ctx;
    FxStatic bg { ctx };
    Nav nav { ctx };
    std::unique_ptr<FxModule> mods[8];
    explicit FxPage (Ctx& c) : ctx (c)
    {
        addAndMakeVisible (bg); bg.setBounds (0, 0, 1120, 840);
        addAndMakeVisible (nav); nav.setTopLeftPosition (1086 - nav.getWidth(), 40); nav.setPage (1);
        for (int i = 0; i < 8; ++i)
        {
            mods[i] = std::make_unique<FxModule> (ctx, i);
            addAndMakeVisible (*mods[i]);
            mods[i]->setBounds (34 + (i % 4) * 268, 150 + (i / 4) * 300, 240, 160);
        }
    }
    void refresh() { for (auto& m : mods) m->refresh(); }
};

// ============================================================ PRESETS page
struct PresetStatic : public Component
{
    Ctx& ctx;
    explicit PresetStatic (Ctx& c) : ctx (c) { setInterceptsMouseClicks (false, false); setBufferedToImage (true); }
    void paint (Graphics& g) override
    {
        auto& t = ctx.theme;
        g.fillAll (t.bg);
        ui::beads (g, t, "PRESETS", 34, 32, 4.4f);
        ui::text3D (g, t, "STARTER", { 34, 120, 200, 16 }, Justification::centredLeft, 11.0f, t.sub, false);
        ui::text3D (g, t, "MY PRESETS", { 34, 330, 200, 16 }, Justification::centredLeft, 11.0f, t.sub, false);
        ui::text3D (g, t, "saved in ~/Music/AquaSynth/Presets", { 34, 790, 320, 14 }, Justification::centredLeft, 10.0f, t.sub, false);
        ui::text3D (g, t, "NAME", { 394, 770, 60, 26 }, Justification::centredLeft, 11.0f, t.sub, false);
    }
};

struct ReportView : public Component
{
    Ctx& ctx; SerumImportResult res; bool has = false; String status;
    explicit ReportView (Ctx& c) : ctx (c) { setInterceptsMouseClicks (false, false); }
    void paint (Graphics& g) override
    {
        auto& t = ctx.theme;
        if (status.isNotEmpty())
            ui::text3D (g, t, status, { 0, 0, (float) getWidth(), 18 }, Justification::centredLeft, 12.0f, t.accentText, false);
        if (! has) return;
        ui::text3D (g, t, res.name, { 0, 28, (float) getWidth(), 22 }, Justification::centredLeft, 16.0f, t.accentText);
        ui::text3D (g, t, res.info, { 0, 52, (float) getWidth(), 16 }, Justification::centredLeft, 11.0f, t.sub, false);
        const StringArray* cols[3] { &res.applied, &res.close, &res.missing };
        const char* titles[3] { "APPLIED", "CLOSE MATCH", "NOT AVAILABLE" };
        const Colour cc[3] { t.wave, t.accentText, t.sub };
        const float cw = ((float) getWidth() - 60) / 3.0f;
        for (int c = 0; c < 3; ++c)
        {
            const float x = (float) c * (cw + 30);
            ui::text3D (g, t, titles[c], { x, 84, cw, 18 }, Justification::centredLeft, 12.0f, cc[c], false);
            g.setFont (ui::dotFont (11.0f));
            float y = 108;
            for (auto& s : *cols[c])
            {
                AttributedString as;
                as.append (s, ui::dotFont (11.0f), t.ink);
                as.setWordWrap (AttributedString::byWord);
                TextLayout tl; tl.createLayout (as, cw);
                tl.draw (g, { x, y, cw, tl.getHeight() + 2 });
                y += tl.getHeight() + 8;
                if (y > (float) getHeight() - 10) break;
            }
        }
    }
};

struct PresetPage : public Component, public FileDragAndDropTarget
{
    Ctx& ctx;
    PresetStatic bg { ctx };
    Nav nav { ctx };
    TextBtn importBtn { ctx, "IMPORT SERUM 2 PRESET" };
    TextBtn importSub { ctx, "drop a .SerumPreset file here, or tap to choose one" };
    std::vector<std::unique_ptr<TextBtn>> starters, mine;
    ReportView report { ctx };
    TextEditor nameBox;
    TextBtn saveBtn { ctx, "SAVE" };
    std::unique_ptr<FileChooser> chooser;
    std::function<void()> onLoaded;
    bool dragging = false;

    explicit PresetPage (Ctx& c) : ctx (c)
    {
        addAndMakeVisible (bg); bg.setBounds (0, 0, 1120, 840);
        addAndMakeVisible (nav); nav.setTopLeftPosition (1086 - nav.getWidth(), 40); nav.setPage (2);
        addAndMakeVisible (importBtn); importBtn.fontSize = 20.0f; importBtn.setBounds (394, 116, 600, 30);
        addAndMakeVisible (importSub); importSub.fontSize = 12.0f; importSub.setBounds (394, 148, 600, 20);
        importBtn.onClick = importSub.onClick = [this] { choose(); };
        addAndMakeVisible (report); report.setBounds (394, 190, 692, 560);
        addAndMakeVisible (nameBox);
        nameBox.setBounds (450, 770, 320, 28);
        nameBox.setTextToShowWhenEmpty ("name this sound", ctx.theme.sub);
        nameBox.setTitle ("Preset name");
        addAndMakeVisible (saveBtn); saveBtn.fontSize = 13.0f; saveBtn.setBounds (790, 772, 120, 26);
        saveBtn.onClick = [this] { save(); };
        const auto names = AquaSynthProcessor::starterNames();
        for (int i = 0; i < names.size(); ++i)
        {
            auto b = std::make_unique<TextBtn> (ctx, names[i]);
            b->fontSize = 13.0f; addAndMakeVisible (*b); b->setBounds (34, 144 + i * 30, 300, 26);
            b->onClick = [this, i] { ctx.proc.loadStarter (i); loaded(); };
            starters.push_back (std::move (b));
        }
        styleEditor();
        refreshList();
    }
    void styleEditor()
    {
        auto& t = ctx.theme;
        nameBox.setFont (ui::dotFont (13.0f));
        nameBox.setColour (TextEditor::backgroundColourId, t.dark ? Colours::white.withAlpha (0.08f) : ui::hex (0x78729A).withAlpha (0.1f));
        nameBox.setColour (TextEditor::textColourId, t.ink);
        nameBox.setColour (TextEditor::outlineColourId, Colours::transparentBlack);
        nameBox.setColour (TextEditor::focusedOutlineColourId, t.wave.withAlpha (0.6f));
        nameBox.setColour (CaretComponent::caretColourId, t.ink);
        nameBox.applyFontToAllText (ui::dotFont (13.0f));
        nameBox.applyColourToAllText (t.ink);
    }
    void refreshList()
    {
        for (auto& b : mine) removeChildComponent (b.get());
        mine.clear();
        auto files = AquaSynthProcessor::presetFolder().findChildFiles (File::findFiles, false, "*.aqpreset");
        files.sort();
        int y = 354;
        for (auto& f : files)
        {
            if (y > 770) break;
            auto b = std::make_unique<TextBtn> (ctx, f.getFileNameWithoutExtension());
            b->fontSize = 13.0f; addAndMakeVisible (*b); b->setBounds (34, y, 300, 26);
            b->onClick = [this, f] { if (ctx.proc.loadPreset (f)) loaded(); };
            mine.push_back (std::move (b));
            y += 28;
        }
        refresh();
    }
    void refresh()
    {
        const auto cur = ctx.proc.currentPresetName;
        for (auto& b : starters) b->set (b->text, b->text == cur);
        for (auto& b : mine) b->set (b->text, b->text == cur);
        importBtn.set (importBtn.text, dragging);
        styleEditor();
        repaint();
    }
    void loaded()
    {
        nameBox.setText (ctx.proc.currentPresetName, false);
        report.status = "loaded " + dot + " " + ctx.proc.currentPresetName;
        report.has = false; report.repaint();
        refresh();
        if (onLoaded) onLoaded();
    }
    void choose()
    {
        chooser = std::make_unique<FileChooser> ("Choose a Serum 2 preset", File::getSpecialLocation (File::userMusicDirectory), "*.SerumPreset");
        chooser->launchAsync (FileBrowserComponent::openMode | FileBrowserComponent::canSelectFiles,
                              [this] (const FileChooser& fc) { auto f = fc.getResult(); if (f.existsAsFile()) importFile (f); });
    }
public:
    void importFile (const File& f)
    {
        auto res = importSerumPreset (f);
        if (! res.ok) { report.status = "could not import: " + res.error; report.has = false; report.repaint(); return; }
        ctx.proc.resetToDefaults();
        ctx.proc.applyValues (res.values);
        for (int o = 0; o < us::numOscs; ++o) ctx.proc.setOscSample (o, res.samples[o]);
        ctx.proc.currentPresetName = res.name.toLowerCase() + " (from serum)";
        report.res = res; report.has = true;
        report.status = "imported " + dot + " play to hear it, then save it below";
        report.repaint();
        nameBox.setText (ctx.proc.currentPresetName, false);
        refresh();
        if (onLoaded) onLoaded();
    }
    void save()
    {
        auto name = File::createLegalFileName (nameBox.getText().trim());
        if (name.isEmpty()) name = "new sound";
        auto f = AquaSynthProcessor::presetFolder().getChildFile (name + ".aqpreset");
        if (ctx.proc.savePreset (f, name)) { report.status = "saved " + dot + " " + name; refreshList(); }
        else report.status = "could not save to " + f.getFullPathName();
        report.repaint();
    }
    bool isInterestedInFileDrag (const StringArray& files) override { return files.size() == 1 && files[0].endsWithIgnoreCase (".SerumPreset"); }
    void fileDragEnter (const StringArray&, int, int) override { dragging = true; refresh(); }
    void fileDragExit (const StringArray&) override { dragging = false; refresh(); }
    void filesDropped (const StringArray& files, int, int) override { dragging = false; importFile (File (files[0])); }
};

// ============================================================ root panel
struct Panel : public Component, private Timer, private AudioProcessorValueTreeState::Listener
{
    Ctx ctx;
    MainPage main { ctx };
    FxPage fx { ctx };
    PresetPage presets { ctx };
    std::atomic<bool> dirty { true };
    int page = 0;

    explicit Panel (AquaSynthProcessor& p) : ctx (p)
    {
        ctx.themeIdx = jlimit (0, 2, p.getTheme());
        ctx.theme = ui::themeFor (ctx.themeIdx);
        setSize (AquaSynthEditor::baseW, AquaSynthEditor::baseH);
        for (auto* pg : { (Component*) &main, (Component*) &fx, (Component*) &presets }) { addChildComponent (*pg); pg->setBounds (getLocalBounds()); }
        main.setVisible (true);
        auto go = [this] (int i) { showPage (i); };
        main.nav.onPage = go; fx.nav.onPage = go; presets.nav.onPage = go;
        main.setTheme = [this] (int i) { setTheme (i); };
        presets.onLoaded = [this] { main.history.clear(); dirty = true; };
        for (auto* prm : p.getParameters())
            if (auto* rp = dynamic_cast<RangedAudioParameter*> (prm))
                ctx.apvts.addParameterListener (rp->getParameterID(), this);
        main.refresh(); fx.refresh();
        startTimerHz (30);
    }
    ~Panel() override
    {
        for (auto* prm : ctx.proc.getParameters())
            if (auto* rp = dynamic_cast<RangedAudioParameter*> (prm))
                ctx.apvts.removeParameterListener (rp->getParameterID(), this);
    }
    void showPage (int i)
    {
        page = i;
        main.setVisible (i == 0); fx.setVisible (i == 1); presets.setVisible (i == 2);
        if (i == 2) presets.refreshList();
        dirty = true;
    }
    void parameterChanged (const String&, float) override { dirty = true; }
    static void repaintAll (Component& c) { c.repaint(); for (auto* ch : c.getChildren()) repaintAll (*ch); }
    void setTheme (int i)
    {
        ctx.themeIdx = i; ctx.theme = ui::themeFor (i);
        ctx.proc.setTheme (i);
        main.refresh(); fx.refresh(); presets.refresh();
        repaintAll (*this);
    }
    float lastBpm = -1; bool lastSynced = false;
    void timerCallback() override
    {
        const float bpm = ctx.proc.hostBpm.load(); const bool syn = ctx.proc.hostSynced.load();
        if (bpm != lastBpm || syn != lastSynced) { lastBpm = bpm; lastSynced = syn; main.lfoHeader.repaint(); dirty = true; }
        if (dirty.exchange (false))
        {
            if (page == 0) main.refresh();
            else if (page == 1) fx.refresh();
        }
    }
};
} // namespace

// ============================================================ editor
AquaSynthEditor::AquaSynthEditor (AquaSynthProcessor& p) : AudioProcessorEditor (&p), proc (p)
{
    panel = std::make_unique<Panel> (p);
    addAndMakeVisible (*panel);
    setResizable (true, true);
    getConstrainer()->setFixedAspectRatio ((double) baseW / (double) baseH);
    setResizeLimits (baseW * 6 / 10, baseH * 6 / 10, baseW * 12 / 10, baseH * 12 / 10);
    setSize (baseW * 8 / 10, baseH * 8 / 10);
}

AquaSynthEditor::~AquaSynthEditor() = default;

void AquaSynthEditor::resized()
{
    const float s = (float) getWidth() / (float) baseW;
    panel->setTransform (AffineTransform::scale (s));
}

void AquaSynthEditor::showPageForTest (int page)
{
    if (auto* p = dynamic_cast<Panel*> (panel.get())) p->showPage (page);
}

void AquaSynthEditor::importForTest (const juce::File& f)
{
    if (auto* p = dynamic_cast<Panel*> (panel.get())) { p->presets.importFile (f); p->showPage (2); }
}
