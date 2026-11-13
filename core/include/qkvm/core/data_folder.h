#pragma once

#include <QString>

namespace qkvm::core {

// Whether the program's data can be read, before anything of it is read or written: the folder of `settingsFile`, the
// list of what is in it, and the file. Empty where they can be read, or are plainly not there (Windows says the file
// or the path is not found: a first start). Otherwise what the person is told: that qKVM cannot read its data, where,
// and Windows' own words; the program then ends, writing nothing. A folder that cannot be read for any other reason
// (a list that shuts the person out, a junction this start may not cross, error 448 under a mitigation it inherited)
// is never taken for an empty one, and never starts a first start.
QString whyDataCannotBeRead(const QString& settingsFile);

} // namespace qkvm::core
