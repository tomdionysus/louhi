//
// Louhi - The Distributed Compute Layer
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Boost.JSON's definitions, compiled once here so the stack needs no compiled
// Boost library, as Lir does. A build that links another library carrying
// them (Lugh's lugh_client) leaves this file out.
#include <boost/json/src.hpp>
