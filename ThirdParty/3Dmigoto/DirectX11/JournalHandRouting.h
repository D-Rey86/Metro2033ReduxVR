#pragma once

struct JournalHandRouting {
	bool independentRightHand;
	bool freezeEmptyHand;
};

// The journal is equipment, not an empty hand. Retain its native grip pose and
// the same viewmodel parent as the book/text. Charger keeps its existing priority.
inline JournalHandRouting GetJournalHandRouting(bool left, bool handOnly,
	bool charger, bool journal, bool lateVisible)
{
	JournalHandRouting routing;
	routing.independentRightHand = !left && (charger || (handOnly && !journal));
	routing.freezeEmptyHand = !left && handOnly && !charger && !lateVisible && !journal;
	return routing;
}
