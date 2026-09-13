#pragma once

// Only the button edge predicts an opening. A book draw confirms continued
// presence, but must never undo an explicit close during its draw-out animation.
namespace JournalPresentation {
    constexpr unsigned OpeningGrace = 180; // longer than the accepted 108-tick pulse
    constexpr unsigned AbsenceGrace = 120; // tolerate short rendering gaps

    inline bool Active(bool requested, unsigned frame, unsigned button, unsigned book)
    {
        return requested && (frame - button <= OpeningGrace || frame - book <= AbsenceGrace);
    }
}
