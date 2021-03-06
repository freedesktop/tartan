/* -*- Mode: C++; indent-tabs-mode: t; c-basic-offset: 8; tab-width: 8 -*- */
/*
 * Tartan
 * Copyright © 2014 Philip Withnall
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
 *     Philip Withnall <philip@tecnocode.co.uk>
 */

/**
 * GVariantVisitor:
 *
 * This is a checker for #GVariant format strings and varargs. For #GVariant
 * methods which accept varargs, it validates the type and nullability of each
 * vararg against the corresponding element in the #GVariant format string (if
 * a constant format string is provided — non-constant format strings cannot be
 * validated, but the user should probably be using #GVariantBuilder directly if
 * they’re dynamically generating a format string).
 *
 * For #GVariant methods with format strings but no varargs, the format string
 * is validated.
 *
 * The format string is parsed and varargs are consumed in parallel. The static
 * type of the varargs is used, so if a weird cast is used (e.g. casting a
 * string literal to an integer and passing it to a ‘u’ format string), no error
 * will be raised. One limitation on the current checker is that the types of
 * #GVariants passed in are not checked. e.g. No error is emitted for the
 * following invalid code:
 *     g_variant_new ('@s', g_variant_new_boolean (FALSE));
 *
 * The checker is quite flexible, and a lot of its behaviour is controlled by
 * the set of #VariantCheckFlags in use for the current part of the parse tree.
 *
 * The error messages produced by this checker should give as much context and
 * guidance towards fixing the problem as possible. Empirically, it seems that
 * the GVariant Format String documentation in GLib’s manual is used quite a
 * lot, since people can’t memorise the format strings. Contextual help in the
 * error messages should try to avoid this.
 *
 * FIXME: Future work could be to implement:
 *  • Reference counting validation of #GVariants (might be better placed in a
 *    general reference counting checker).
 *  • #GVariant print format parsing (for g_variant_new_parsed() and
 *    g_variant_builder_add_parsed()).
 *  • Character-granularity error diagnostic locations, e.g. pointing to the
 *    erroneous character in a format string, not just to the start of the
 *    format string argument itself.
 *
 * FIXME: If Clang’s DiagnosticsEngine gains support for multiple
 * SourceLocations, it would be great to highlight both the relevant character
 * of the #GVariant format string, and the erroneous variadic arguments in the
 * function call, when an error is printed. At the moment we have to just pick
 * the most important of the two and highlight that.
 */

#include <cstring>

#include <clang/AST/Attr.h>

#include <glib.h>

#include "debug.h"
#include "gvariant-checker.h"

namespace tartan {

/* Information about the GVariant functions we’re interested in. If you want to
 * add support for a new GVariant function, it may be enough to add a new
 * element here. */
typedef struct {
	/* C name of the function */
	const char *func_name;
	/* Zero-based index of the GVariant format string parameter to the
	 * function; the validity of this string will be checked */
	unsigned int format_param_index;
	/* Zero-based index of the first varargs parameter or va_list. */
	unsigned int first_vararg_param_index;
	/* Whether the function takes a va_list instead of varargs. */
	bool uses_va_list;
	/* True if the argument direction is in; false if it’s out. */
	bool args_in;
} VariantFuncInfo;

static const VariantFuncInfo gvariant_format_funcs[] = {
	{ "g_variant_new", 0, 1, false, true },
	{ "g_variant_new_va", 0, 2, true, true },
	{ "g_variant_get", 1, 2, false, false },
	{ "g_variant_get_va", 1, 3, true, false },
	{ "g_variant_get_child", 2, 3, false, false },
	{ "g_variant_lookup", 2, 3, false, false },
	{ "g_variant_iter_next", 1, 2, false, false },
	{ "g_variant_iter_loop", 1, 2, false, false },
	{ "g_variant_builder_add", 1, 2, false, true },
};

/**
 * VariantCheckFlags:
 * @CHECK_FLAG_NONE: No flags.
 * @CHECK_FLAG_FORCE_GVARIANT: Force the expected type of the next variadic
 * argument to be consumed to be GVariant*.
 * @CHECK_FLAG_FORCE_VALIST: Force the expected type of the next variadic
 * argument to be consumed to be va_list*.
 * @CHECK_FLAG_REQUIRE_CONST: Require that the pointee of the expected type (if
 * it is a pointer type) must be constant. This is ignored it
 * %CHECK_FLAG_DIRECTION_OUT is not set.
 * @CHECK_FLAG_DIRECTION_OUT: Expect the argument to be out-bound, so an extra
 * level of pointer indirection will be expected on the expected type. If
 * %CHECK_FLAG_ALLOW_MAYBE is also set, the top-most pointer can be %NULL.
 * @CHECK_FLAG_ALLOW_MAYBE: Allow the next variadic argument to be consumed to
 * be potentially %NULL. This always examines the top-most argument, not the
 * value it points to if it’s a pointer.
 * @CHECK_FLAG_CONSUME_ARGS: Consume variadic arguments when parsing. If this
 * is not specified, the argument pointer will never be advanced, and all
 * GVariant format strings for a given call will be checked against the same
 * function argument.
 *
 * Flags affecting the parser and checker’s behaviour.
 */
typedef enum {
	CHECK_FLAG_NONE = 0,
	CHECK_FLAG_FORCE_GVARIANT = 1 << 0,
	CHECK_FLAG_FORCE_VALIST = 1 << 2,
	CHECK_FLAG_REQUIRE_CONST = 1 << 4,
	CHECK_FLAG_DIRECTION_OUT = 1 << 5,
	CHECK_FLAG_ALLOW_MAYBE = 1 << 6,
	CHECK_FLAG_CONSUME_ARGS = 1 << 7,
} VariantCheckFlags;


static const VariantFuncInfo *
_func_uses_gvariant_format (const FunctionDecl& func)
{
	const std::string func_name = func.getNameAsString ();
	guint i;

	/* Fast path elimination of irrelevant functions. */
	if (func_name[0] != 'g')
		return NULL;

	for (i = 0; i < G_N_ELEMENTS (gvariant_format_funcs); i++) {
		if (func_name == gvariant_format_funcs[i].func_name)
			return &gvariant_format_funcs[i];
	}

	return NULL;
}

/*
 * Return true if @actual_type and @expected_type compare equal, taking
 * qualifications into account as specified by @flags.
 *
 * Check that @actual_type and @expected_type are equal. For inbound arguments,
 * we need to compare the unqualified (with const, volatile, restrict removed)
 * types, plus the unqualified pointee types if the normal types are pointers,
 * plus the unqualified pointee pointee types, etc.
 *
 * .e.g.
 *    char* ≡ const char*
 *    int ≡ int
 *    char* ≡ char*
 *    GVariant* ≡ const GVariant*
 *    char** ≡ const char * const *
 *
 * For outbound arguments, we must compare qualified types.
 */
static bool
_compare_types (const QualType actual_type, const QualType expected_type,
                unsigned int /* VariantCheckFlags */ flags, ASTContext& context)
{
	DEBUG ("Comparing type ‘" << actual_type.getAsString () << "’ with ‘" <<
	       expected_type.getAsString () << "’.");

	/* Fast path: Simple comparison. */
	if (context.hasSameType (actual_type, expected_type)) {
		return true;
	}

	/* Slow path: Strip pointers off and remove qualifiers for inbound
	 * actual types. */
	const PointerType *actual_pointer_type = dyn_cast<PointerType> (actual_type);
	const PointerType *expected_pointer_type = dyn_cast<PointerType> (expected_type);

	if (actual_pointer_type == NULL || expected_pointer_type == NULL) {
		return false;
	}

	QualType actual_pointee_type, expected_pointee_type;

	actual_pointee_type = actual_pointer_type->getPointeeType ();
	expected_pointee_type = expected_pointer_type->getPointeeType ();

	/* Inbound arguments can be const or not. It’s a bit trickier for
	 * outbound arguments. */
	if (!(flags & CHECK_FLAG_DIRECTION_OUT)) {
		actual_pointee_type = actual_pointee_type.getUnqualifiedType ();
	}

	return _compare_types (actual_pointee_type, expected_pointee_type,
	                       flags, context);
}

/*
 * Return true if the given @type is known to differ in width on different
 * operating systems or processor architectures. This is important for
 * portability, as otherwise the static analysis is only testing correctness
 * for the current platform.
 *
 * For example,
 *    g_variant_get (x, "i", &some_long)
 * is valid on 32-bit machines (where long is 32 bits wide), but invalid on
 * 64-bit machines (where it is 64 bits wide). We want to flag the problem
 * regardless of whether the analyser is run on a 32- or 64-bit host.
 */
static bool
_type_is_arch_dependent (const QualType type, ASTContext &context)
{
	/* Strip off any pointers. */
	const PointerType *pointer_type = dyn_cast<PointerType> (type);

	if (pointer_type != NULL) {
		return _type_is_arch_dependent (pointer_type->getPointeeType (),
		                                context);
	}

	/* If it’s a typedef type, assume it’s not architecture dependent.
	 * This is a tricky one, but is required because the Clang type system
	 * ignores typedefs and preprocessor statements when comparing types, so
	 *     hasSameType(gint64, long)
	 * returns true, just the same as
	 *     hasSameType (long, long)
	 * return true. We want to avoid g* basic types (such as gint64) being
	 * considered as architecture-dependent, since they carefully use
	 * preprocessor voodoo to avoid that.
	 *
	 * So, assume that if the programmer has used an architecture-dependent
	 * type in a typedef, they know enough to make the typedef
	 * architecture-dependent.
	 *
	 * But glong is a typedef, so we have to special case that. Sigh. */
	const TypedefType *typedef_type = dyn_cast<TypedefType> (type);

	if (typedef_type != NULL) {
		const std::string typedef_name =
			typedef_type->getDecl ()->getNameAsString ();
		return (typedef_name == "glong" ||
		        typedef_name == "gulong");
	}

	/* Well-known architecture-dependent types.
	 *
	 * Reference: https://software.intel.com/en-us/articles/
	 *            size-of-long-integer-type-on-different-architecture-and-os
	 */
	return (context.hasSameType (type, context.LongTy) ||
	        context.hasSameType (type, context.UnsignedLongTy) ||
	        context.hasSameType (type, context.LongDoubleTy));
}

/* Consume a single variadic argument from the varargs array, checking that one
 * exists and has the given @expected_type.
 *
 * Iff %CHECK_FLAG_ALLOW_MAYBE is set, the variadic argument may be NULL.
 *
 * This will emit errors where found. */
static bool
_consume_variadic_argument (QualType expected_type,
                            CallExpr::const_arg_iterator *args_begin,
                            CallExpr::const_arg_iterator *args_end,
                            unsigned int /* VariantCheckFlags */ flags,
                            CompilerInstance& compiler,
                            const StringLiteral *format_arg_str,
                            ASTContext& context, TypeManager &type_manager)
{
	/* If the GVariant method doesn’t use varargs, don’t actually consume
	 * the argument. */
	if (!(flags & CHECK_FLAG_CONSUME_ARGS)) {
		return true;
	}

	/* In certain parsing states the expected types have been modified by a
	 * preceding character in the format string. Force the expected types in
	 * those cases. */
	if (flags & CHECK_FLAG_FORCE_GVARIANT) {
		expected_type = type_manager.find_pointer_type_by_name ("GVariant");
	} else if (flags & CHECK_FLAG_FORCE_VALIST) {
		expected_type = type_manager.find_pointer_type_by_name ("va_list");
	}

	/* Handle const-ness of out arguments. We have to insert the const one
	 * layer of pointer indirection down. i.e. char* becomes const char*. */
	if ((flags & CHECK_FLAG_DIRECTION_OUT) &&
	    (flags & CHECK_FLAG_REQUIRE_CONST) &&
	    expected_type->isPointerType ()) {
		const PointerType *expected2_type = dyn_cast<PointerType> (expected_type);
		QualType expected_pointee_type = expected2_type->getPointeeType ();
		expected_pointee_type = context.getConstType (expected_pointee_type);
		expected_type = context.getPointerType (expected_pointee_type);
	}

	/* Handle in/out arguments. This must be done after constness. */
	if ((flags & CHECK_FLAG_DIRECTION_OUT) &&
	    !(flags & CHECK_FLAG_FORCE_VALIST)) {
		expected_type = context.getPointerType (expected_type);
	}

	DEBUG ("Consuming variadic argument with expected type ‘" <<
	       expected_type.getAsString () << "’.");

	if (*args_begin == *args_end) {
		Debug::emit_error ("Expected a GVariant variadic argument of "
		                   "type %0 but there wasn’t one.", compiler,
#ifdef HAVE_LLVM_8_0
		                   format_arg_str->getBeginLoc ()
#else
		                   format_arg_str->getLocStart ()
#endif
		                   )
		<< expected_type;

		return false;
	}

	const Expr *arg = **args_begin;

	/* Check its nullability. */
	QualType actual_type = arg->getType ();
	bool is_null_constant = arg->isNullPointerConstant (context,
	                                                    Expr::NPC_ValueDependentIsNull);

	/* Check for int → uint promotions. */
	llvm::APSInt int_constant_value;
	bool is_int_constant = arg->isIntegerConstantExpr (int_constant_value,
	                                                   context);

	if (is_int_constant && int_constant_value.isNonNegative () &&
	    expected_type->isUnsignedIntegerType () &&
	    actual_type->hasSignedIntegerRepresentation ()) {
		/* Magically promote the int to a uint. */
		actual_type = context.getCorrespondingUnsignedType (actual_type);
	}

	if (is_null_constant && !(flags & CHECK_FLAG_ALLOW_MAYBE) &&
	    expected_type->isPointerType ()) {
		Debug::emit_error ("Expected a GVariant variadic argument of "
		                   "type %0 but saw NULL instead.", compiler,
#ifdef HAVE_LLVM_8_0
		                   arg->getBeginLoc ()
#else
		                   arg->getLocStart ()
#endif
		                   )
		<< expected_type;

		return false;
	} else if (!is_null_constant) {
		/* Normal case. */
		bool type_error = !_compare_types (actual_type, expected_type,
		                                   flags, context);
		bool arch_error = _type_is_arch_dependent (actual_type,
		                                           context);

		if (arch_error) {
			Debug::emit_error ("Expected a GVariant variadic "
			                   "argument of type %0 but saw one "
			                   "of type %1. These types are not "
			                   "compatible on every architecture.",
			                   compiler,
#ifdef HAVE_LLVM_8_0
			                   arg->getBeginLoc ()
#else
			                   arg->getLocStart ()
#endif
			                   )
			<< expected_type
			<< actual_type;

			return false;
		} else if (type_error) {
			Debug::emit_error ("Expected a GVariant variadic "
			                   "argument of type %0 but saw one "
			                   "of type %1.", compiler,
#ifdef HAVE_LLVM_8_0
			                   arg->getBeginLoc ()
#else
			                   arg->getLocStart ()
#endif
			                   )
			<< expected_type
			<< actual_type;

			return false;
		}
	}

	/* Consume the format. */
	*args_begin = *args_begin + 1;

	return true;
}

/* Parse a single basic type string from the beginning of the string pointed to
 * by @type_str (i.e. *type_str). Consume any variadic parameters from
 * @args_begin as appropriate. This will emit errors where found.
 *
 * @type_str and @args_begin are updated as the type string and arguments are
 * consumed. */
static bool
_check_basic_type_string (const gchar **type_str,
                          CallExpr::const_arg_iterator *args_begin,
                          CallExpr::const_arg_iterator *args_end,
                          unsigned int /* VariantCheckFlags */ flags,
                          CompilerInstance& compiler,
                          const StringLiteral *format_arg_str,
                          ASTContext& context, TypeManager &type_manager)
{
	DEBUG ("Checking basic type string ‘" << *type_str << "’.");

	QualType expected_type;

	/* Reference: GVariant Type Strings. */
	switch (**type_str) {
	/* Numeric Types */
	case 'b': /* gboolean ≡ gint ≡ int */
		expected_type = context.IntTy;
		break;
	case 'y': /* guchar ≡ unsigned char */
		expected_type = context.UnsignedCharTy;
		break;
	case 'n': /* gint16 */
		expected_type = type_manager.find_type_by_name ("gint16");
		break;
	case 'q': /* guint16 */
		expected_type = type_manager.find_type_by_name ("guint16");
		break;
	case 'i':
	case 'h': /* gint32 */
		expected_type = type_manager.find_type_by_name ("gint32");
		break;
	case 'u': /* guint32 */
		expected_type = type_manager.find_type_by_name ("guint32");
		break;
	case 'x': /* gint64 */
		expected_type = type_manager.find_type_by_name ("gint64");
		break;
	case 't': /* guint64 */
		expected_type = type_manager.find_type_by_name ("guint64");
		break;
	case 'd': /* gdouble ≡ double */
		expected_type = context.DoubleTy;
		break;
	/* Strings */
	case 's':
	case 'o':
	case 'g': /* gchar* ≡ char* */
		/* FIXME: Could also validate o and g as D-Bus object paths and
		 * type signatures. */
		expected_type = context.getPointerType (context.CharTy);
		break;
	/* Basic types */
	case '?': /* GVariant* of any type */
		expected_type = type_manager.find_pointer_type_by_name ("GVariant");
		break;
	default:
		Debug::emit_error ("Expected a GVariant basic type string but "
		                   "saw ‘%0’.", compiler,
#ifdef HAVE_LLVM_8_0
		                   format_arg_str->getBeginLoc ()
#else
		                   format_arg_str->getLocStart ()
#endif
		                   )
		<< std::string (1, **type_str);

		return false;
	}

	assert (!expected_type.isNull ());

	/* Handle type promotion. Integer types which are smaller than 32 bits
	 * (for all architectures we care about) are automatically promoted to
	 * 32 bits when passed as varargs.
	 *
	 * A subtlety of the standard (ISO/IEC 9899, §6.3.1.1¶2) means that all
	 * types are promoted to *signed* 32-bit integers. This is because int
	 * can represent all values representable by 16-bit (and smaller)
	 * unsigned integers.
	 *
	 * References:
	 *  • GVariant Format Strings, §Numeric Types
	 *  • ISO/IEC 9899, §6.5.2.2¶6
	 */
	if (!(flags & CHECK_FLAG_DIRECTION_OUT) &&
	    (**type_str == 'y' || **type_str == 'n' || **type_str == 'q')) {
		assert (expected_type->isPromotableIntegerType ());
		expected_type = context.getPromotedIntegerType (expected_type);
	}

	/* Consume the type string. */
	*type_str = *type_str + 1;

	return _consume_variadic_argument (expected_type,
	                                   args_begin, args_end,
	                                   flags, compiler,
	                                   format_arg_str, context,
	                                   type_manager);
}

/* Parse a single type string from the beginning of the string pointed to
 * by @type_str (i.e. *type_str). Consume any variadic parameters from
 * @args_begin as appropriate. This will emit errors where found.
 *
 * @type_str and @args_begin are updated as the type string and arguments are
 * consumed. */
static bool
_check_type_string (const gchar **type_str,
                    CallExpr::const_arg_iterator *args_begin,
                    CallExpr::const_arg_iterator *args_end,
                    unsigned int /* VariantCheckFlags */ flags,
                    CompilerInstance& compiler,
                    const StringLiteral *format_arg_str,
                    ASTContext& context, TypeManager &type_manager)
{
	DEBUG ("Checking type string ‘" << *type_str << "’.");

	QualType expected_type;

	/* Reference: GVariant Type Strings. */
	switch (**type_str) {
	/* Variants */
	case 'v': /* GVariant* */
		expected_type = type_manager.find_pointer_type_by_name ("GVariant");
		break;
	/* Arrays */
	case 'a':
		/* Consume the ‘a’. */
		*type_str = *type_str + 1;

		/* Update flags for the array element type.
		 *
		 * FIXME: ALLOW_MAYBE only for definite types */
		flags |= CHECK_FLAG_ALLOW_MAYBE;

		if (flags & CHECK_FLAG_DIRECTION_OUT) {
			expected_type = type_manager.find_pointer_type_by_name ("GVariantIter");
		} else {
			expected_type = type_manager.find_pointer_type_by_name ("GVariantBuilder");
		}

		/* Check and consume the type string for the array element
		 * type. */
		if (!_check_type_string (type_str, args_begin, args_end,
		                         flags & ~CHECK_FLAG_CONSUME_ARGS,
		                         compiler, format_arg_str, context,
		                         type_manager)) {
			return false;
		}

		/* Consume the single GVariantBuilder for the array. */
		return _consume_variadic_argument (expected_type,
		                                   args_begin, args_end,
		                                   flags, compiler,
		                                   format_arg_str, context,
		                                   type_manager);
	/* Maybe Types */
	case 'm':
		*type_str = *type_str + 1;  /* consume the ‘m’ */
		return _check_type_string (type_str, args_begin, args_end,
		                           flags | CHECK_FLAG_ALLOW_MAYBE,
		                           compiler, format_arg_str, context,
		                           type_manager);
	/* Tuples */
	case '(':
		*type_str = *type_str + 1;  /* consume the opening bracket */

		while (**type_str != ')' && **type_str != '\0') {
			if (!_check_type_string (type_str, args_begin,
			                         args_end,
			                         flags, compiler,
			                         format_arg_str, context,
			                         type_manager)) {
				return false;
			}
		}

		if (**type_str != ')') {
			Debug::emit_error ("Invalid GVariant type string: "
			                   "tuple did not end with ‘)’.",
			                   compiler,
#ifdef HAVE_LLVM_8_0
			                   format_arg_str->getBeginLoc ()
#else
			                   format_arg_str->getLocStart ()
#endif
			                   );
			return false;
		}

		*type_str = *type_str + 1;  /* consume the closing bracket */

		return true;
	case 'r': /* GVariant* of tuple type */
		/* FIXME: Validate that the GVariant* has a tuple type. */
		expected_type = type_manager.find_pointer_type_by_name ("GVariant");
		break;
	/* Dictionaries */
	case '{':
		*type_str = *type_str + 1;  /* consume the opening brace */

		if (**type_str == '}') {
			Debug::emit_error (
				"Invalid GVariant type string: dict did not "
				"contain exactly two elements.",
				compiler,
#ifdef HAVE_LLVM_8_0
				format_arg_str->getBeginLoc ()
#else
				format_arg_str->getLocStart ()
#endif
				);
			return false;
		} else if (!_check_basic_type_string (type_str, args_begin,
		                                      args_end,
		                                      flags, compiler,
		                                      format_arg_str, context,
		                                      type_manager)) {
			return false;
		}

		if (**type_str == '}') {
			Debug::emit_error (
				"Invalid GVariant type string: dict did not "
				"contain exactly two elements.",
				compiler,
#ifdef HAVE_LLVM_8_0
				format_arg_str->getBeginLoc ()
#else
				format_arg_str->getLocStart ()
#endif
				);
			return false;
		} else if (!_check_type_string (type_str, args_begin, args_end,
		                                flags, compiler,
		                                format_arg_str, context,
		                                type_manager)) {
			return false;
		}

		if (**type_str == '\0') {
			Debug::emit_error (
				"Invalid GVariant type string: dict "
				"did not end with ‘}’.",
				compiler,
#ifdef HAVE_LLVM_8_0
				format_arg_str->getBeginLoc ()
#else
				format_arg_str->getLocStart ()
#endif
				);
			return false;
		} else if (**type_str != '}') {
			Debug::emit_error (
				"Invalid GVariant type string: dict "
				"contains more than two elements.",
				compiler,
#ifdef HAVE_LLVM_8_0
				format_arg_str->getBeginLoc ()
#else
				format_arg_str->getLocStart ()
#endif
				);
			return false;
		}

		*type_str = *type_str + 1;  /* consume the closing brace */

		return true;
	/* GVariant* */
	case '*': /* GVariant* of any type */
		expected_type = type_manager.find_pointer_type_by_name ("GVariant");
		break;
	default:
		/* Fall back to checking basic types. */
		return _check_basic_type_string (type_str, args_begin, args_end,
		                                 flags, compiler,
		                                 format_arg_str, context,
		                                 type_manager);
	}

	/* Consume the type string. */
	*type_str = *type_str + 1;

	return _consume_variadic_argument (expected_type,
	                                   args_begin, args_end,
	                                   flags, compiler,
	                                   format_arg_str, context,
	                                   type_manager);
}

/* Parse a single basic format string from the beginning of the string pointed
 * to by @format_str (i.e. *format_str). Consume any variadic parameters from
 * @args_begin as appropriate. This will emit errors where found.
 *
 * @format_str and @args_begin are updated as the format string and arguments
 * are consumed. */
static bool
_check_basic_format_string (const gchar **format_str,
                            CallExpr::const_arg_iterator *args_begin,
                            CallExpr::const_arg_iterator *args_end,
                            unsigned int /* VariantCheckFlags */ flags,
                            CompilerInstance& compiler,
                            const StringLiteral *format_arg_str,
                            ASTContext& context, TypeManager &type_manager)
{
	DEBUG ("Checking format string ‘" << *format_str << "’.");

	/* Reference: GVariant Format Strings documentation, §Syntax. */
	switch (**format_str) {
	case '@':
		*format_str = *format_str + 1;  /* consume the ‘@’ */
		return _check_basic_type_string (format_str, args_begin,
		                                 args_end,
		                                 flags |
		                                 CHECK_FLAG_FORCE_GVARIANT,
		                                 compiler, format_arg_str,
		                                 context, type_manager);
	case '?':
		/* Direct GVariant. */
		*format_str = *format_str + 1;  /* consume the argument */
		return _consume_variadic_argument (type_manager.find_pointer_type_by_name ("GVariant"),
		                                   args_begin, args_end,
		                                   flags,
		                                   compiler, format_arg_str,
		                                   context, type_manager);
	case '&':
		/* Ignore it for inbound arguments; require that outbound
		 * arguments are const. */
		*format_str = *format_str + 1;
		return _check_basic_type_string (format_str, args_begin,
		                                 args_end,
		                                 flags |
		                                 CHECK_FLAG_REQUIRE_CONST,
		                                 compiler, format_arg_str,
		                                 context, type_manager);
	case '^': {
		/* Various different hard-coded types. */
		*format_str = *format_str + 1;

		QualType expected_type;
		QualType char_array = context.getPointerType (context.CharTy);
		QualType const_char_array = context.getPointerType (context.getConstType (context.CharTy));
		guint skip;

		/* Effectively hard-code the table from
		 * §Convenience Conversions. */
#define CONVENIENCE_FORMAT(STR_PTR, F) (strncmp (STR_PTR, F, strlen (F)) == 0)
		if (CONVENIENCE_FORMAT (*format_str, "as") ||
		    CONVENIENCE_FORMAT (*format_str, "ao")) {
			expected_type = context.getPointerType (char_array);
			skip = 2;
		} else if (CONVENIENCE_FORMAT (*format_str, "a&s") ||
		           CONVENIENCE_FORMAT (*format_str, "a&o")) {
			expected_type = context.getPointerType (const_char_array);
			skip = 3;
		} else if (CONVENIENCE_FORMAT (*format_str, "aay")) {
			expected_type = context.getPointerType (char_array);
			skip = 3;
		} else if (CONVENIENCE_FORMAT (*format_str, "ay")) {
			expected_type = char_array;
			skip = 2;
		} else if (CONVENIENCE_FORMAT (*format_str, "&ay")) {
			expected_type = const_char_array;
			skip = 3;
		} else if (CONVENIENCE_FORMAT (*format_str, "a&ay")) {
			expected_type = context.getPointerType (const_char_array);
			skip = 4;
		} else {
			Debug::emit_error (
				"Invalid GVariant basic format string: "
				"convenience operator ‘^’ was not followed by "
				"a recognized convenience conversion.",
				compiler,
#ifdef HAVE_LLVM_8_0
				format_arg_str->getBeginLoc ()
#else
				format_arg_str->getLocStart ()
#endif
				);
			return false;
		}
#undef CONVENIENCE_FORMAT

		*format_str = *format_str + skip;

		return _consume_variadic_argument (expected_type, args_begin,
		                                   args_end,
		                                   flags, compiler,
		                                   format_arg_str, context,
		                                   type_manager);
	}
	default:
		/* Assume it’s a type string. */
		return _check_basic_type_string (format_str, args_begin,
		                                 args_end,
		                                 flags, compiler,
		                                 format_arg_str, context,
		                                 type_manager);
	}
}

/* Parse a single format string from the beginning of the string pointed
 * to by @format_str (i.e. *format_str). Consume any variadic parameters from
 * @args_begin as appropriate. This will emit errors where found.
 *
 * @format_str and @args_begin are updated as the format string and arguments
 * are consumed. */
static bool
_check_format_string (const gchar **format_str,
                      CallExpr::const_arg_iterator *args_begin,
                      CallExpr::const_arg_iterator *args_end,
                      unsigned int /* VariantCheckFlags */ flags,
                      CompilerInstance& compiler,
                      const StringLiteral *format_arg_str,
                      ASTContext& context, TypeManager &type_manager)
{
	DEBUG ("Checking format string ‘" << *format_str << "’.");

	/* Reference: GVariant Format Strings documentation, §Syntax. */
	switch (**format_str) {
	case '@':
		*format_str = *format_str + 1;  /* consume the ‘@’ */
		return _check_type_string (format_str, args_begin, args_end,
		                           flags | CHECK_FLAG_FORCE_GVARIANT,
		                           compiler, format_arg_str, context,
		                           type_manager);
	case 'm':
		*format_str = *format_str + 1;  /* consume the ‘m’ */
		return _check_format_string (format_str, args_begin, args_end,
		                             flags | CHECK_FLAG_ALLOW_MAYBE,
		                             compiler, format_arg_str, context,
		                             type_manager);
	case '*':
	case '?':
	case 'r':
		/* Direct GVariants. */
		*format_str = *format_str + 1;  /* consume the argument */
		return _consume_variadic_argument (type_manager.find_pointer_type_by_name ("GVariant"),
		                                   args_begin, args_end,
		                                   flags,
		                                   compiler, format_arg_str,
		                                   context, type_manager);
	case '(':
		*format_str = *format_str + 1;  /* consume the opening bracket */

		while (**format_str != ')' && **format_str != '\0') {
			if (!_check_format_string (format_str, args_begin,
			                           args_end,
			                           flags, compiler,
			                           format_arg_str, context,
			                           type_manager)) {
				return false;
			}
		}

		if (**format_str != ')') {
			Debug::emit_error (
				"Invalid GVariant format string: tuple "
				"did not end with ‘)’.",
				compiler,
#ifdef HAVE_LLVM_8_0
				format_arg_str->getBeginLoc ()
#else
				format_arg_str->getLocStart ()
#endif
				);
			return false;
		}

		*format_str = *format_str + 1;  /* consume the closing bracket */

		return true;
	case '{':
		*format_str = *format_str + 1;  /* consume the opening brace */

		if (**format_str == '}') {
			Debug::emit_error (
				"Invalid GVariant format string: dict did not "
				"contain exactly two elements.",
				compiler,
#ifdef HAVE_LLVM_8_0
				format_arg_str->getBeginLoc ()
#else
				format_arg_str->getLocStart ()
#endif
				);
			return false;
		} else if (!_check_basic_format_string (format_str, args_begin,
		                                        args_end,
		                                        flags, compiler,
		                                        format_arg_str,
		                                        context,
		                                        type_manager)) {
			return false;
		}

		if (**format_str == '}') {
			Debug::emit_error (
				"Invalid GVariant format string: dict did not "
				"contain exactly two elements.",
				compiler,
#ifdef HAVE_LLVM_8_0
				format_arg_str->getBeginLoc ()
#else
				format_arg_str->getLocStart ()
#endif
				);
			return false;
		} else if (!_check_format_string (format_str, args_begin,
		                                  args_end,
		                                  flags, compiler,
		                                  format_arg_str, context,
		                                  type_manager)) {
			return false;
		}

		if (**format_str == '\0') {
			Debug::emit_error (
				"Invalid GVariant format string: dict "
				"did not end with ‘}’.",
				compiler,
#ifdef HAVE_LLVM_8_0
				format_arg_str->getBeginLoc ()
#else
				format_arg_str->getLocStart ()
#endif
				);
			return false;
		} else if (**format_str != '}') {
			Debug::emit_error (
				"Invalid GVariant format string: dict "
				"contains more than two elements.",
				compiler,
#ifdef HAVE_LLVM_8_0
				format_arg_str->getBeginLoc ()
#else
				format_arg_str->getLocStart ()
#endif
				);
			return false;
		}

		*format_str = *format_str + 1;  /* consume the closing brace */

		return true;
	case '&':
		/* Ignore it for inbound arguments; require that outbound
		 * arguments are const. */
		*format_str = *format_str + 1;
		return _check_type_string (format_str, args_begin, args_end,
		                           flags | CHECK_FLAG_REQUIRE_CONST,
		                           compiler, format_arg_str, context,
		                           type_manager);
	case '^':
		/* Handled by the basic format string parser. */
		return _check_basic_format_string (format_str, args_begin,
		                                   args_end,
		                                   flags, compiler,
		                                   format_arg_str, context,
		                                   type_manager);
	default:
		/* Assume it’s a type string. */
		return _check_type_string (format_str, args_begin, args_end,
		                           flags, compiler,
		                           format_arg_str, context,
		                           type_manager);
	}
}

/* Build a GVariant format string to represent the given type, or return NULL if
 * no representation is known. The returned value must be freed using
 * g_free(). */
static gchar *
_gvariant_format_string_for_type (QualType type,
                                  ASTContext &context,
                                  TypeManager &type_manager)
{
	const BuiltinType *bt = dyn_cast<BuiltinType> (type);

	/* Boolean. */
	if ((bt != NULL && bt->getKind () == BuiltinType::Bool) ||
	    type == type_manager.find_pointer_type_by_name ("gboolean")) {
		return g_strdup ("b");
	    } else if (bt != NULL && bt->getKind () == BuiltinType::UChar) {
		return g_strdup ("y");
	} else if (bt != NULL && bt->getKind () == BuiltinType::LongDouble) {
		return g_strdup ("d");
	} else if (type->isSignedIntegerType ()) {
		uint64_t size = context.getTypeSize (type);

		if (size == 16) {
			return g_strdup ("n");
		} else if (size == 32) {
			return g_strdup ("i");
		} else if (size == 64) {
			return g_strdup ("x");
		}
	} else if (type->isUnsignedIntegerType ()) {
		uint64_t size = context.getTypeSize (type);

		if (size == 16) {
			return g_strdup ("q");
		} else if (size == 32) {
			return g_strdup ("u");
		} else if (size == 64) {
			return g_strdup ("t");
		}
	} else if (type->isPointerType ()) {
		const QualType pointee_type = type->getPointeeType ();

		if (pointee_type->isCharType () && pointee_type.isConstQualified ()) {
			return g_strdup ("&s");  /* or 'o' or 'g' */
		} else if (pointee_type->isCharType ()) {
			return g_strdup ("s");
		} else if (pointee_type == type_manager.find_pointer_type_by_name ("GVariant")) {
			return g_strdup ("v");
		} else if (pointee_type->isPointerType ()) {
			const QualType pointee2_type = pointee_type->getPointeeType ();

			if (pointee2_type->isCharType ()) {
				/* const gchar * const * */
				return g_strdup ("^as");
			}
		}
	}

	/* FIXME: Add support for:
	 *  - Arrays
	 *  - Maybe types
	 *  - Tuples
	 */
	return NULL;
}

/* Check a GVariant function call which passes a format parameter. Validate the
 * format parameter string, and if the function takes varargs, validate their
 * types against that parameter.
 *
 * If the format string is not a string literal, we can’t check anything. */
static bool
_check_gvariant_format_param (const CallExpr& call,
                              const FunctionDecl &func,
                              const VariantFuncInfo *func_info,
                              CompilerInstance& compiler,
                              ASTContext& context, TypeManager &type_manager)
{
	/* Grab the format parameter string. */
	const Expr *format_arg = call.getArg (func_info->format_param_index)->IgnoreParenImpCasts ();

	DEBUG ("Checking GVariant format strings in " << func.getNameAsString () << "().");

	const StringLiteral *format_arg_str = dyn_cast<StringLiteral> (format_arg);
	if (format_arg_str == NULL) {
		Debug::emit_warning (
			"Non-literal GVariant format string in call to %0(). "
			"Cannot check format string correctness. Instead "
			"of a non-literal format string, use GVariantBuilder.",
			compiler,
#ifdef HAVE_LLVM_8_0
			format_arg->getBeginLoc ()
#else
			format_arg->getLocStart ()
#endif
			)
		<< func.getNameAsString ();
		return false;
	}

	/* Check the string. Parse it hand-in-hand with iterating through the
	 * varargs list. Take a copy of the format_str because StringRef.str()
	 * only uses a temporary internal buffer. */
	DEBUG ("Checking GVariant format string ‘" <<
	       format_arg_str->getString () << "’ with " <<
	       call.getNumArgs () << " variadic arguments.");

	gchar *whole_format_str;
	whole_format_str = g_strdup (format_arg_str->getString ().str ().c_str ());
	const gchar *format_str = whole_format_str;
	CallExpr::const_arg_iterator args_begin = call.arg_begin ();
	CallExpr::const_arg_iterator args_end = call.arg_end ();

	/* Skip up to the varargs. If args_begin points to a va_list, the rest
	 * of the code will ignore it. */
	for (unsigned int i = 0; i < func_info->first_vararg_param_index; i++)
		++args_begin;

	unsigned int flags = CHECK_FLAG_NONE;
	if (!func_info->uses_va_list)
		flags |= CHECK_FLAG_CONSUME_ARGS;
	else
		flags |= CHECK_FLAG_FORCE_VALIST;

	/* Outbound arguments may always be NULL to skip that GVariant
	 * element. */
	if (!func_info->args_in)
		flags |= (CHECK_FLAG_DIRECTION_OUT | CHECK_FLAG_ALLOW_MAYBE);

	if (!_check_format_string (&format_str, &args_begin, &args_end,
	                           flags, compiler, format_arg_str, context,
	                           type_manager)) {
		g_free (whole_format_str);
		return false;
	}

	/* Sanity check that we’ve consumed all format strings. If not, the
	 * user has probably forgotten to add tuple brackets around their format
	 * string. Don’t emit any error messages about unpaired variadic
	 * arguments because that would just confuse things. */
	if (*format_str != '\0') {
		Debug::emit_error ("Unexpected GVariant format strings ‘%0’ "
		                   "with unpaired arguments. If using multiple "
		                   "format strings, they should be enclosed in "
		                   "brackets to create a tuple (e.g. ‘(%1)’).",
		                   compiler,
#ifdef HAVE_LLVM_8_0
		                   format_arg_str->getBeginLoc ()
#else
		                   format_arg_str->getLocStart ()
#endif
		                   )
		<< format_str
		<< whole_format_str;

		g_free (whole_format_str);

		return false;
	}

	g_free (whole_format_str);

	/* Sanity check that we’ve consumed all arguments. */
	bool retval = true;

	for (; !func_info->uses_va_list && args_begin != args_end;
	     ++args_begin) {
		const Expr *arg = *args_begin;
		gchar *error_format_str;

		error_format_str = _gvariant_format_string_for_type (arg->getType (),
		                                                     context,
		                                                     type_manager);

		if (error_format_str != NULL) {
			Debug::emit_error ("Unexpected GVariant variadic "
			                   "argument of type %0. Either it "
			                   "should be removed, or a ‘%1’ "
			                   "(or other valid) GVariant format "
			                   "string should be added to the "
			                   "format argument to use it.",
			                   compiler,
#ifdef HAVE_LLVM_8_0
			                   arg->getBeginLoc ()
#else
			                   arg->getLocStart ()
#endif
			                   )
			<< arg->getType ()
			<< error_format_str;
		} else {
			Debug::emit_error ("Unexpected GVariant variadic "
			                   "argument of type %0. Either it "
			                   "should be removed, or a GVariant "
			                   "format string should be added to "
			                   "the format argument to use it. "
			                   "There is no known GVariant "
			                   "representation of the argument’s "
			                   "type, so the argument must be "
			                   "serialized to a "
			                   "GVariant-representable type first.",
			                   compiler,
#ifdef HAVE_LLVM_8_0
			                   arg->getBeginLoc ()
#else
			                   arg->getLocStart ()
#endif
			                   )
			<< arg->getType ();
		}

		retval = false;
	}

	return retval;
}

void
GVariantConsumer::HandleTranslationUnit (ASTContext& context)
{
	/* Run away if the plugin is disabled. */
	if (!this->is_enabled ()) {
		return;
	}

	this->_visitor.TraverseDecl (context.getTranslationUnitDecl ());
}

/* Note: Specifically overriding the Traverse* method here to re-implement
 * recursion to child nodes. */
bool
GVariantVisitor::VisitCallExpr (CallExpr* expr)
{
	const VariantFuncInfo *func_info;

	/* Can only handle direct function calls (i.e. not calling dereferenced
	 * function pointers). */
	const FunctionDecl *func = expr->getDirectCallee ();
	if (func == NULL)
		return true;

	/* We’re only interested in functions which handle GVariants. */
	func_info = _func_uses_gvariant_format (*func);
	if (func_info == NULL)
		return true;

	/* Check the format parameter. */
	_check_gvariant_format_param (*expr, *func, func_info, this->_compiler,
	                              func->getASTContext (),
	                              this->_type_manager);

	return true;
}

} /* namespace tartan */
