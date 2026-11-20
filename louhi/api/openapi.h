//
// Louhi - The Distributed Compute Layer
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <boost/json/object.hpp>

// The OpenAPI 3.1 document for all of /v1/ (dashboard design, "The API the
// dashboard reads"): its paths, methods and scopes from the route table
// (routes.h), its bodies from schemas written here by hand beside json.cpp,
// which writes the records they describe. Every route needs a bearer token,
// this document's own included.
namespace louhi::api {

// Built anew on each call: the Api builds it once and answers that.
boost::json::object openapi_document();

}  // namespace louhi::api
