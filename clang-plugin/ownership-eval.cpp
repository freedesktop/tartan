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

/**
 * OwnershipEval:
 *
 * This is a checker for #GError usage, both with the g_error_*() API, and with
 * normal C pointer operations on #GErrors. It validates that all #GError
 * pointers are initialised to %NULL, that valid #GErrors are not overwritten,
 * and that #GErrors are not double-freed or leaked. It also validates more
 * mundane things, like whether error codes actually belong in the domain passed
 * to g_error_new() (for example).
 *
 * This is effectively a highly specific memory allocation checker, imposing the
 * rules about clearing #GError pointers to %NULL which #GError convention
 * dictates.
 *
 * The checker uses full path-dependent analysis, so will catch bugs arising
 * from #GErrors being handled differently on different control paths, which is
 * empirically where most #GError bugs arise.
 *
 * The checker is implemented using a combination of Clang’s internal symbolic
 * value model, and a custom ErrorMap using Clang’s program state maps. The
 * ErrorMap tracks state for each GError* pointer it knows about, using three
 * states:
 *  • Clear: error = NULL
 *  • Set: error ≠ NULL ∧ valid_allocation(error)
 *  • Freed: error ≠ NULL ∧ ¬valid_allocation(error)
 *
 * In the comments below, the following modelling functions are used:
 *     valid_allocation(error):
 *         True iff error has been allocated as a GError, but not yet freed.
 *         Corresponds to the ErrorState.Set state.
 *     error_codes(domain):
 *         Returns a set of error codes which are valid for the given domain,
 *         as defined by the enum associated with that error domain.
 *
 * FIXME: Future work could be to implement:
 *  • Support for user-defined functions which take GError** parameters.
 *  • Add support for g_error_copy()
 *  • Add support for g_error_matches()
 *  • Add support for g_prefix_error()
 *  • Implement check::DeadSymbols  (for cleaning up internal state)
 *  • Implement check::PointerEscape  (for leaks)
 *  • Implement check::ConstPointerEscape  (for leaks)
 *  • Implement check::PreStmt<ReturnStmt>  (for leaks)
 *  • Implement check::PostStmt<BlockExpr>  (for leaks)
 *  • Implement check::Location  (for bad dereferences)
 *  • Implement eval::Assume
 *  • Check that error codes match their domains.
 *  • Set the MemRegion contents more explicitly in _gerror_new() — it would be
 *    nice to get static analysis on code and domain values.
 *  • Domain analysis on propagated GErrors: track which error domains each
 *    function can possibly return, and warn if they’re not all handled by
 *    callers.
 *
 * TODO
 */

#include <clang/StaticAnalyzer/Core/Checker.h>
#include <clang/StaticAnalyzer/Core/PathSensitive/CallEvent.h>
#include <clang/StaticAnalyzer/Core/PathSensitive/CheckerContext.h>

#include "ownership-eval.h"
#include "type-manager.h"
#include "debug.h"
// TODO

namespace tartan {

/* Dispatch call-evaluation events to the different per-function handlers.
 * Return true iff the call was evaluated. */
bool
OwnershipEval::evalCall (const CallExpr *call,
                         CheckerContext &context) const
{
	const FunctionDecl *func_decl = context.getCalleeDecl (call);

	if (func_decl == NULL ||
	    func_decl->getKind() != Decl::Function ||
	    !CheckerContext::isCLibraryFunction (func_decl)) {
		return false;
	}

	/* Try to find typelib information about the function. */
	const std::string func_name = func_decl->getNameAsString ();
	GIBaseInfo *info =
		this->_gir_manager.get ()->find_function_info (func_name);

	if (info == NULL ||
	    g_base_info_get_type (info) != GI_INFO_TYPE_FUNCTION) {
		return false;
	}

	/* Evaluate the call. */
	ProgramStateRef new_state = this->_evaluate_gir_call (context, *call,
	                                                      info);

	if (new_state != NULL) {
		context.addTransition (new_state);
	}

	return (new_state != NULL);
}

static bool
is_callback_sval (SVal sval, QualType type) {
	// If the parameter is 0, it's harmless.
	if (sval.isZeroConstant ()) {
		return false;
	}

	// If a parameter is a block or a callback, assume it can modify pointer.
	if (type->isFunctionPointerType ()) {
		return true;
	}

	// Check if a callback is passed inside a struct (for both, struct passed by
	// reference and by value). Dig just one level into the struct for now.
	if (type->isAnyPointerType () || type->isReferenceType ()) {
		type = type->getPointeeType ();
	}

	if (const RecordType *record_type = type->getAsStructureType ()) {
		const RecordDecl *record = record_type->getDecl ();
		for (const auto *i : record->fields ()) {
			QualType field_type = i->getType ();

			if (field_type->isFunctionPointerType ()) {
				return true;
			}
		}
	}

	return false;
}

static bool
has_callback_args (CheckerContext &context,
                   const CallExpr &call_expr)
{
	unsigned int num_args = call_expr.getNumArgs ();
	unsigned int idx = 0;

	for (CallExpr::const_arg_iterator i = call_expr.arg_begin (),
	     e = call_expr.arg_end ();
	     i != e && idx < num_args; ++i, ++idx) {
		if (num_args <= idx) {
			break;
		}

		SVal arg_sval = context.getSVal (call_expr.getArg (idx));

		if (is_callback_sval (arg_sval, (*i)->getType ())) {
			return true;
		}
	}

	return false;
}

static bool
args_may_escape (CheckerContext &context,
                 const CallExpr &call_expr)
{
	return has_callback_args (context, call_expr);
}

// Try to retrieve the function declaration and find the function parameter
// types which are pointers/references to a non-pointer const.
// We will not invalidate the corresponding argument regions.
// @info may be %NULL.
static void
preserve_const_args (llvm::SmallSet<unsigned int, 4> &preserve_args,
                     const CallExpr &call, GIFunctionInfo *info)
{
	unsigned int idx = 0;

	for (CallExpr::const_arg_iterator i = call.arg_begin (),
	     e = call.arg_end ();
	     i != e; ++i, ++idx) {
		QualType type = (*i)->getType ();
		QualType pointee_type = type->getPointeeType ();

		if (pointee_type != QualType () &&
		    pointee_type.isConstQualified () &&
		    !pointee_type->isAnyPointerType ()) {
			preserve_args.insert (idx);
		}
	}

	/* Load the GIR info, iterate through the arguments and mark all
	 * of them as to be preserved, except those with (transfer !none)
	 * (direction in|out). */
	GICallableInfo *callable_info = (GICallableInfo *) info;

	/* GError formal parameters aren’t included in the number of
	 * callable arguments. */
	unsigned int k = g_callable_info_get_n_args (callable_info);
	unsigned int self_params =
		(g_function_info_get_flags (callable_info) &
		 GI_FUNCTION_IS_METHOD) ? 1 : 0;
	unsigned int j;

	/* TODO: unit test this */
	for (j = self_params; j < k + self_params; j++) {
		GIArgInfo arg;
		GITransfer transfer;

		g_callable_info_load_arg (callable_info,
		                          j - self_params, &arg);
		transfer = g_arg_info_get_ownership_transfer (&arg);

		if (transfer == GI_TRANSFER_NOTHING) {
			preserve_args.insert (j);
		}
	}
}

/* TODO */
ProgramStateRef
OwnershipEval::_evaluate_gir_call (CheckerContext &context,
                                   const CallExpr &call_expr,
                                   GIFunctionInfo *info) const
{
	ProgramStateRef state = context.getState ();
	const FunctionDecl *callee = call_expr.getDirectCallee ();
	unsigned int count = context.blockCount ();

	/* Invalidate regions. Don’t invalidate anything if the callee is pure
	 * or const. */
	if (callee == NULL ||
	    !(callee->hasAttr<PureAttr> () || callee->hasAttr<ConstAttr> ())) {
		SmallVector<SVal, 8> values_to_invalidate;
		RegionAndSymbolInvalidationTraits traits;

		/* Indexes of arguments whose values will be preserved by the
		 * call. */
		llvm::SmallSet<unsigned int, 4> preserve_args;
		if (!args_may_escape (context, call_expr))
			preserve_const_args (preserve_args, call_expr, info);

		for (unsigned int i = 0, j = call_expr.getNumArgs ();
		     i != j; i++) {
			SVal arg_sval = context.getSVal (call_expr.getArg (i));

			// Mark this region for invalidation.  We batch invalidate regions
			// below for efficiency.
			const MemRegion *region = arg_sval.getAsRegion ();

			if (preserve_args.count (i) && region != NULL) {
				traits.setTrait (region->StripCasts (),
				                 RegionAndSymbolInvalidationTraits::TK_PreserveContents);
			}

			values_to_invalidate.push_back (arg_sval);
		}

		// Invalidate designated regions using the batch invalidation API.
		// NOTE: Even if RegionsToInvalidate is empty, we may still invalidate
		//  global variables.
  // Get the call in its initial state. We use this as a template to perform
  // all the checks.
  CallEventManager &CEMgr = context.getStateManager().getCallEventManager();
  CallEventRef<> CallTemplate
    = CEMgr.getSimpleCall(&call_expr, state, context.getLocationContext());

		state = state->invalidateRegions (values_to_invalidate,
		                                  &call_expr, count,
		                                  context.getLocationContext (),
		                                  /*CausedByPointerEscape*/ true,
		                                  /*Symbols=*/ NULL, NULL  /* TODO */,
		                                  &traits);
	}

	/* Bind a new return value. */
	if (callee != NULL) {
		// Conjure a symbol if the return value is unknown.
		QualType result_type = callee->getReturnType ();
		SValBuilder &sval_builder = context.getSValBuilder ();
		SVal sval = sval_builder.conjureSymbolVal (NULL, &call_expr,
		                                           context.getLocationContext (),
		                                           result_type, count);
		state = state->BindExpr (&call_expr,
		                         context.getLocationContext (), sval);
	}

	return state;
}

} /* namespace tartan */
