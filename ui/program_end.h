#pragma once

namespace qkvm::ui {

class OwnWall;
class HandOverEnd;

// The program's end, while its loop can still run (main's aboutToQuit): the sessions of servers' controllers given
// back, each within its own time, as the web closes each device at its shutdown. Every driver of the wall and of its
// boxes is let go of, none made anew and nothing of the wall looking or signing in from then on; their deletions are
// sent now (the loop has ended: a turn of it later each), so that each hands its session to the goodbyes and a
// server's screen says its console's goodbye as its driver goes (its web page's session given back from there); and
// only then are those waited for (the reading of 1a-v, L4; M7 1b-i and 1c). A start into BIOS setup under way has its
// override cleared and read back first (bounded: Pace::setupClearWithinMs, waited for by finish itself), and is noted
// beside the settings until that is seen (review 4's MEDIUM 1). Then a JetKVM's letting go of what its connection had
// down. And only once all of that is over, where the program has handed over to Setup, its forced end is counted
// from there (HandOverEnd::goodbyesOver): it never cuts a goodbye or a clearing short.
void endTheProgram(OwnWall& own, HandOverEnd& handOver);

} // namespace qkvm::ui
