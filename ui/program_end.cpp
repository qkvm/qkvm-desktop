#include "program_end.h"

#include <QCoreApplication>
#include <QEvent>

#include <qkvm/core/jetkvm_connection.h>
#include <qkvm/core/redfish_device.h>

#include "own_wall.h"
#include "win/hand_over_end.h"

namespace qkvm::ui {

void endTheProgram(OwnWall& own, HandOverEnd& handOver)
{
    own.letGoOfEverything();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    core::ControllerGoodbyes::program().finish(core::RedfishDevice::Pace{}.signOutWithinMs);
    // And a JetKVM's letting go of what its connection had down (a button held in a drag) is given its time, whether
    // or not a server's goodbye was waited for above (review 3's LOW 4): bounded by the longest a connection takes to
    // let go (the answers after a hand-over, then the letting go's own time), at once where none lets go.
    const core::JetKvmConnection::Pace jetkvm;
    core::JetKvmConnection::finishLettingGo(jetkvm.handOverGraceMs + jetkvm.letGoWaitMs + jetkvm.letGoSettleMs);
    handOver.goodbyesOver();
}

} // namespace qkvm::ui
