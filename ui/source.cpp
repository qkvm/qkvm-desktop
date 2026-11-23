#include "source.h"

#include <qkvm/core/server_client.h>

namespace qkvm::ui {

core::MediaPlace* Source::mediaPlace()
{
    // (A server's are had through its client, where it has one: ServerClient is a MediaPlace.)
    return detailsClient();
}

} // namespace qkvm::ui
