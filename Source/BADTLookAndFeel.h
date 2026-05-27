// =============================================================================
// BADTLookAndFeel.h  —  BADT custom JUCE LookAndFeel_V4
// =============================================================================
#pragma once
#include <JuceHeader.h>

class BADTLookAndFeel : public juce::LookAndFeel_V4
{
public:
    BADTLookAndFeel()
    {
        setColour(juce::Label::textColourId,             juce::Colour(0xFF333333));
        setColour(juce::Slider::textBoxTextColourId,     juce::Colour(0xFF333333));
        setColour(juce::Slider::textBoxOutlineColourId,  juce::Colours::transparentBlack);
        setColour(juce::Slider::textBoxBackgroundColourId, juce::Colour(0xFFF5F6F8));
        setColour(juce::ToggleButton::textColourId,      juce::Colour(0xFF333333));
    }

    // =========================================================================
    // drawRotarySlider — Glassy white knob with bipolar center-tracking glow
    //                    LFO knobs have linear glow (0 at left, full at right)
    // =========================================================================
    void drawRotarySlider(juce::Graphics& g,
                          int x, int y, int width, int height,
                          float sliderPosProportional,
                          float rotaryStartAngle,
                          float rotaryEndAngle,
                          juce::Slider& slider) override
    {
        const float cx     = x + width  * 0.5f;
        const float cy     = y + height * 0.5f;
        const float radius = std::min(width, height) * 0.5f - 8.0f;

        if (radius <= 2.0f) return;

        // Calculate angles
        const float angleRange = rotaryEndAngle - rotaryStartAngle;
        const float centerAngle = rotaryStartAngle + (angleRange * 0.5f);
        const float currentAngle = rotaryStartAngle + (sliderPosProportional * angleRange);

        // LFO knobs are unipolar (0=off, 1=full); all others bipolar (centre=off).
        const bool isLFOKnob = slider.getComponentID() == "lfo-rate" || slider.getComponentID() == "lfo-depth";
        const float distFromCenter = isLFOKnob ? sliderPosProportional
                                               : (std::abs(sliderPosProportional - 0.5f) * 2.0f);

        // ----- 1. Dynamic Glow (diffuse radial, within component bounds) -----
        if (distFromCenter > 0.02f)
        {
            // Colour ramp: hue 0.33 = green → 0.17 = yellow → 0.0 = red.
            const float hue     = 0.33f * (1.0f - distFromCenter);
            const float alpha   = distFromCenter * 0.60f;
            const auto  glowCol = juce::Colour::fromHSV(hue, 0.90f, 1.0f, 1.0f);

            // Radial gradient stays within the component bounds — never clips.
            const float glowEdge = static_cast<float>(std::min(width, height)) * 0.5f;
            const float knobFrac = radius / glowEdge;  // ~0.8 for standard knobs

            // Inner halo (tight ring at knob rim)
            juce::ColourGradient inner(
                glowCol.withAlpha(0.0f),         cx, cy,
                glowCol.withAlpha(0.0f),         cx + glowEdge, cy,
                true);
            inner.addColour(std::max(0.0f, knobFrac - 0.30f), glowCol.withAlpha(0.0f));
            inner.addColour(knobFrac,                          glowCol.withAlpha(alpha));
            g.setGradientFill(inner);
            g.fillEllipse(static_cast<float>(x), static_cast<float>(y),
                          static_cast<float>(width), static_cast<float>(height));

            // Outer bloom (wider, softer pass for diffuse spread)
            juce::ColourGradient bloom(
                glowCol.withAlpha(alpha * 0.35f), cx, cy,
                glowCol.withAlpha(0.0f),          cx + glowEdge, cy,
                true);
            g.setGradientFill(bloom);
            g.fillEllipse(static_cast<float>(x), static_cast<float>(y),
                          static_cast<float>(width), static_cast<float>(height));
        }

        // ----- 2. Track arc (full range, dark grey recess) -----
        juce::Path track;
        track.addCentredArc(cx, cy, radius, radius, 0.0f, rotaryStartAngle, rotaryEndAngle, true);
        g.setColour(juce::Colour(0xFFD0D4D9)); // Light metallic recess
        g.strokePath(track, juce::PathStrokeType(3.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        
        // Inner dark shadow for track depth
        g.setColour(juce::Colour(0x44000000));
        g.strokePath(track, juce::PathStrokeType(1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        // ----- 3. Value arc (centre → current position) -----
        if (distFromCenter > 0.01f)
        {
            juce::Path valueArc;
            valueArc.addCentredArc(cx, cy, radius, radius, 0.0f, centerAngle, currentAngle, true);

            // Match the glow colour: green → yellow → red
            const float arcHue = 0.33f * (1.0f - distFromCenter);
            const auto  arcCol = juce::Colour::fromHSV(arcHue, 0.80f, 0.95f, 1.0f);

            g.setColour(arcCol);
            g.strokePath(valueArc, juce::PathStrokeType(3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }

        // ----- 4. Knob Body Drop Shadow -----
        g.setColour(juce::Colours::black.withAlpha(0.2f));
        g.fillEllipse(cx - radius * 0.75f, (cy - radius * 0.75f) + 3.0f, radius * 1.5f, radius * 1.5f);

        // ----- 5. Knob body (Glassy White Sphere) -----
        const float highlightOffsetX = -radius * 0.3f;
        const float highlightOffsetY = -radius * 0.3f;

        juce::ColourGradient bodyGrad(
            juce::Colour(0xFFFFFFFF), cx + highlightOffsetX, cy + highlightOffsetY,
            juce::Colour(0xFFC0C7D0), cx + radius * 0.6f, cy + radius * 0.6f, true);
        bodyGrad.addColour(0.6, juce::Colour(0xFFEAEEF2));

        g.setGradientFill(bodyGrad);
        g.fillEllipse(cx - radius * 0.78f, cy - radius * 0.78f, radius * 1.56f, radius * 1.56f);

        // ----- 6. Glass rim crescent reflection -----
        g.setColour(juce::Colours::white.withAlpha(0.9f));
        g.drawEllipse(cx - radius * 0.78f + 1.0f, cy - radius * 0.78f + 1.0f, 
                      (radius * 1.56f) - 2.0f, (radius * 1.56f) - 2.0f, 1.5f);

        // ----- 7. Pointer line (Sleek Dark Indent) -----
        const float pointerLen    = radius * 0.55f;
        const float pointerOffset = radius * 0.20f;
        juce::Path pointer;
        pointer.addRoundedRectangle(-1.5f, -pointerOffset - pointerLen, 3.0f, pointerLen, 1.5f);
        
        juce::AffineTransform t = juce::AffineTransform::rotation(currentAngle).translated(cx, cy);
        g.setColour(juce::Colour(0xFF2C3E50)); // Dark metallic grey
        g.fillPath(pointer, t);
    }

    // =========================================================================
    // drawToggleButton — 3D Metallic Rocker (A/P) or flat BYP/SOLO button
    // =========================================================================
    void drawToggleButton(juce::Graphics& g,
                          juce::ToggleButton& button,
                          bool /*shouldDrawHighlighted*/,
                          bool shouldDrawAsDown) override
    {
        const auto bounds  = button.getLocalBounds().toFloat().reduced(1.0f);
        const bool toggled = button.getToggleState();
        const auto id      = button.getComponentID();

        if (id == "ap")
        {
            // ---- 3D Metallic Rocker Switch ----
            const float corner = bounds.getHeight() * 0.4f;
            
            // Outer bezel housing
            g.setColour(juce::Colour(0xFF9098A0));
            g.fillRoundedRectangle(bounds, corner);
            g.setColour(juce::Colours::black.withAlpha(0.3f));
            g.drawRoundedRectangle(bounds, corner, 1.0f);

            // Inner Rocker bounds
            auto innerBounds = bounds.reduced(2.0f);
            
            // Draw the two sides of the rocker
            auto leftHalf = innerBounds.withRight(innerBounds.getCentreX());
            auto rightHalf = innerBounds.withLeft(innerBounds.getCentreX());

            // A side (Left) is pressed if NOT toggled. P side (Right) is pressed if toggled.
            bool aIsDown = !toggled;

            // Gradient for pressed side (darker) vs raised side (lighter, catches light)
            juce::ColourGradient downGrad(juce::Colour(0xFF50555A), 0, innerBounds.getY(), juce::Colour(0xFF30353A), 0, innerBounds.getBottom(), false);
            juce::ColourGradient upGrad(juce::Colour(0xFFE0E5EA), 0, innerBounds.getY(), juce::Colour(0xFFA0A5AA), 0, innerBounds.getBottom(), false);

            // Draw A (Left)
            g.setGradientFill(aIsDown ? downGrad : upGrad);
            g.fillRoundedRectangle(leftHalf.getX(), leftHalf.getY(), leftHalf.getWidth() + 2.0f, leftHalf.getHeight(), corner - 1.0f);
            
            // Draw P (Right)
            g.setGradientFill(!aIsDown ? downGrad : upGrad);
            g.fillRoundedRectangle(rightHalf.getX() - 2.0f, rightHalf.getY(), rightHalf.getWidth() + 2.0f, rightHalf.getHeight(), corner - 1.0f);

            // Labels with LED glow
            const auto font = juce::Font(juce::FontOptions().withHeight(10.0f).withStyle("Bold"));
            g.setFont(font);

            // 'A' Label (Amber glow when pressed)
            g.setColour(aIsDown ? juce::Colour(0xFFFFC030) : juce::Colour(0xFF70757A));
            g.drawFittedText("A", leftHalf.toNearestInt(), juce::Justification::centred, 1);

            // 'P' Label (Blue glow when pressed)
            g.setColour(!aIsDown ? juce::Colour(0xFF40A0FF) : juce::Colour(0xFF70757A));
            g.drawFittedText("P", rightHalf.toNearestInt(), juce::Justification::centred, 1);
        }
        else
        {
            // ---- Flat text button: BYP (red accent) or SOLO/INV (amber accent) ----
            const float corner    = 4.0f;
            const bool  isByp     = (id == "byp");
            const auto  accentOn  = isByp ? juce::Colour(0xFFE74C3C) : juce::Colour(0xFFF1C40F);
            const auto  bgCol     = toggled ? accentOn.withAlpha(0.2f) : juce::Colour(0xFF34495E);
            g.setColour(bgCol);
            g.fillRoundedRectangle(bounds, corner);

            if (shouldDrawAsDown)
            {
                g.setColour(juce::Colours::black.withAlpha(0.15f));
                g.fillRoundedRectangle(bounds, corner);
            }

            g.setColour(toggled ? accentOn : juce::Colour(0xFF2C3E50));
            g.drawRoundedRectangle(bounds, corner, 1.5f);

            g.setColour(toggled ? accentOn.brighter(0.4f) : juce::Colours::white);
            g.setFont(juce::Font(juce::FontOptions().withHeight(10.0f).withStyle("Bold")));
            g.drawFittedText(button.getButtonText(), button.getLocalBounds(), juce::Justification::centred, 1);
        }
    }

    // =========================================================================
    // drawLinearSlider — Glassy rounded thumb on a recessed track
    // =========================================================================
    void drawLinearSlider(juce::Graphics& g,
                          int x, int y, int width, int height,
                          float sliderPos,
                          float /*minSliderPos*/,
                          float maxSliderPos,
                          juce::Slider::SliderStyle style,
                          juce::Slider& slider) override
    {
        if (style == juce::Slider::LinearVertical || style == juce::Slider::LinearHorizontal)
        {
            const bool isVert = (style == juce::Slider::LinearVertical);
            const float trackW  = 6.0f; 
            const float thumbW  = isVert ? 24.0f : 16.0f;
            const float thumbH  = isVert ? 16.0f : 24.0f;

            if (isVert)
            {
                const float tx = x + width * 0.5f - trackW * 0.5f;
                
                // Track Background
                g.setColour(juce::Colour(0xFFC0C7D0));
                g.fillRoundedRectangle(tx, (float)y, trackW, (float)height, trackW * 0.5f);
                g.setColour(juce::Colour(0x33000000));
                juce::Path trackPath;
                trackPath.addRoundedRectangle(tx, (float)y, trackW, (float)height, trackW * 0.5f);
                g.strokePath(trackPath, juce::PathStrokeType(1.0f));

                // Filled Section (Blue-ish gradient)
                juce::ColourGradient fillGrad(juce::Colour(0xFF40A0FF), 0, sliderPos, juce::Colour(0xFF2268B0), 0, maxSliderPos, false);
                g.setGradientFill(fillGrad);
                g.fillRoundedRectangle(tx, sliderPos, trackW, maxSliderPos - sliderPos, trackW * 0.5f);

                // Thumb Drop Shadow
                const float tx2 = x + width * 0.5f - thumbW * 0.5f;
                const float ty  = sliderPos - thumbH * 0.5f;
                g.setColour(juce::Colours::black.withAlpha(0.15f));
                g.fillRoundedRectangle(tx2, ty + 2.0f, thumbW, thumbH, 4.0f);

                // Glassy Thumb
                juce::ColourGradient thumbGrad(
                    juce::Colour(0xFFFFFFFF), tx2, ty,
                    juce::Colour(0xFFBEC8D4), tx2 + thumbW, ty + thumbH, false);
                g.setGradientFill(thumbGrad);
                g.fillRoundedRectangle(tx2, ty, thumbW, thumbH, 4.0f);
                
                // Thumb border & reflection
                g.setColour(juce::Colour(0x55000000));
                g.drawRoundedRectangle(tx2, ty, thumbW, thumbH, 4.0f, 1.0f);
                g.setColour(juce::Colours::white.withAlpha(0.8f));
                g.drawRoundedRectangle(tx2 + 1.0f, ty + 1.0f, thumbW - 2.0f, thumbH - 2.0f, 3.0f, 1.0f);
            }
            else
            {
                const float ty = y + height * 0.5f - trackW * 0.5f;
                g.setColour(juce::Colour(0xFFC0C7D0));
                g.fillRoundedRectangle((float)x, ty, (float)width, trackW, trackW * 0.5f);

                g.setColour(juce::Colour(0xFF40A0FF));
                g.fillRoundedRectangle((float)x, ty, sliderPos - (float)x, trackW, trackW * 0.5f);

                const float tx = sliderPos - thumbW * 0.5f;
                const float ty2 = y + height * 0.5f - thumbH * 0.5f;
                
                g.setColour(juce::Colours::black.withAlpha(0.15f));
                g.fillRoundedRectangle(tx, ty2 + 2.0f, thumbW, thumbH, 4.0f);

                juce::ColourGradient thumbGrad(juce::Colour(0xFFFFFFFF), tx, ty2, juce::Colour(0xFFBEC8D4), tx + thumbW, ty2 + thumbH, false);
                g.setGradientFill(thumbGrad);
                g.fillRoundedRectangle(tx, ty2, thumbW, thumbH, 4.0f);
                
                g.setColour(juce::Colour(0x55000000));
                g.drawRoundedRectangle(tx, ty2, thumbW, thumbH, 4.0f, 1.0f);
            }
        }
        else
        {
            LookAndFeel_V4::drawLinearSlider(g, x, y, width, height, sliderPos, 0, maxSliderPos, style, slider);
        }
    }
};