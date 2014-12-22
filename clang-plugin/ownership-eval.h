/* -*- Mode: C++; indent-tabs-mode: t; c-basic-offset: 8; tab-width: 8 -*- */
/*
 * Tartan
 * Copyright © 2014 Collabora Ltd.
 *
 * Tartan is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Tartan is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Tartan.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Authors:
 *     Philip Withnall <philip.withnall@collabora.co.uk>
 */

#ifndef TARTAN_OWNERSHIP_EVAL_H
#define TARTAN_OWNERSHIP_EVAL_H

#include <clang/AST/AST.h>
#include <clang/StaticAnalyzer/Core/Checker.h>
#include <clang/StaticAnalyzer/Core/PathSensitive/CallEvent.h>
#include <clang/StaticAnalyzer/Core/PathSensitive/CheckerContext.h>

#include "checker.h"
#include "gir-manager.h"

namespace tartan {

using namespace clang;
using namespace ento;

class OwnershipEval : public ento::Checker<eval::Call>,
                      public tartan::Checker {
public:
	explicit OwnershipEval () : _gir_manager (global_gir_manager) {};

protected:
	std::shared_ptr<const GirManager> _gir_manager;

private:
	ProgramStateRef _evaluate_gir_call (CheckerContext &context,
	                                    const CallExpr &call_expr,
	                                    GIFunctionInfo *info) const;

public:
	bool evalCall (const CallExpr *call,
	               CheckerContext &context) const;

	const std::string get_name () const { return "ownership"; }
};

} /* namespace tartan */

#endif /* !TARTAN_OWNERSHIP_EVAL_H */
