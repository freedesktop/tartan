/* -*- Mode: C++; indent-tabs-mode: t; c-basic-offset: 8; tab-width: 8 -*- */
/*
 * Tartan
 * Copyright © 2013, 2014 Collabora Ltd.
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

#include "config.h"

#include <cstring>

#include <girepository.h>
#include <gitypes.h>

#include <clang/AST/Attr.h>

#include "debug.h"
#include "gir-attributes.h"

namespace tartan {

/* Determine whether a type should be const, given its (transfer) annotation and
 * base type. */
static bool
_type_should_be_const (GITransfer transfer, GITypeTag type_tag)
{
	return (transfer == GI_TRANSFER_NOTHING &&
	        (type_tag == GI_TYPE_TAG_UTF8 ||
	         type_tag == GI_TYPE_TAG_FILENAME ||
	         type_tag == GI_TYPE_TAG_ARRAY ||
	         type_tag == GI_TYPE_TAG_GLIST ||
	         type_tag == GI_TYPE_TAG_GSLIST ||
	         type_tag == GI_TYPE_TAG_GHASH ||
	         type_tag == GI_TYPE_TAG_ERROR));
}

/* Determine whether an argument is definitely required to be non-NULL given
 * its (nullable) and (optional) annotations, direction annotation and type.
 *
 * If it’s an array type, it may be NULL if its associated length parameter is
 * 0. Since we can’t currently analyse array bounds, assume that all C array
 * parameters may be NULL. (Other array types are structs, so may not be
 * NULL.) */
static bool
_arg_is_nonnull (GIArgInfo arg, GITypeInfo type_info)
{
	return ((g_type_info_is_pointer (&type_info) ||
	         g_arg_info_get_direction (&arg) == GI_DIRECTION_OUT) &&
	        !g_arg_info_may_be_null (&arg) &&
	        !g_arg_info_is_optional (&arg) &&
	        !(g_type_info_get_tag (&type_info) == GI_TYPE_TAG_ARRAY &&
	          g_type_info_get_array_type (&type_info) == GI_ARRAY_TYPE_C));
}

/* Determine whether a return type is constant. Typically, this will be used
 * for constant pointer types, in which case pointer_type will be non-NULL. */
static bool
_function_return_type_is_const (FunctionDecl& func)
{
#ifdef HAVE_LLVM_3_5
	QualType type = func.getReturnType ();
#else /* if !HAVE_LLVM_3_5 */
	QualType type = func.getResultType ();
#endif /* !HAVE_LLVM_3_5 */

	type = type.getDesugaredType (func.getASTContext ());

	const PointerType* pointer_type = dyn_cast<PointerType> (type);
	if (pointer_type == NULL)
		return type.isConstQualified ();

	return (pointer_type->getPointeeType ().isConstQualified () || type.isConstQualified ());
}

/* Make the return type of a FunctionType const. This will go one level of
 * typing below the return type, so it won’t constify the top-level pointer
 * return. e.g.:
 *     char* → const char *          (pointer to const char)
 * and not:
 *     char* → char * const          (const pointer to char)
 *     char* → const char * const    (const pointer to const char) */
static void
_constify_function_return_type (FunctionDecl& func)
{
	/* We have to construct a new type because the existing FunctionType
	 * is immutable. */
	const FunctionType* f_type = func.getType ()->getAs<FunctionType> ();
	ASTContext& context = func.getASTContext ();
#ifdef HAVE_LLVM_3_5
	const QualType old_result_type = f_type->getReturnType ();
#else /* if !HAVE_LLVM_3_5 */
	const QualType old_result_type = f_type->getResultType ();
#endif /* !HAVE_LLVM_3_5 */

	const PointerType* old_result_pointer_type = dyn_cast<PointerType> (old_result_type);
	if (old_result_pointer_type == NULL)
		return;

	QualType new_result_pointee_type =
		old_result_pointer_type->getPointeeType ().withConst ();
	QualType new_result_type = context.getPointerType (new_result_pointee_type);

	for (FunctionDecl* func_decl = func.getMostRecentDecl ();
	     func_decl != NULL; func_decl = func_decl->getPreviousDecl ()) {
		const FunctionNoProtoType* f_n_type =
			dyn_cast<FunctionNoProtoType> (f_type);
		QualType t;

		if (f_n_type != NULL) {
			t = context.getFunctionNoProtoType (new_result_type,
			                                    f_n_type->getExtInfo ());
		} else {
			const FunctionProtoType *f_p_type =
				cast<FunctionProtoType> (f_type);
			ArrayRef<QualType> param_types;

#ifdef HAVE_LLVM_3_5
			param_types = f_p_type->getParamTypes ();
#else /* !HAVE_LLVM_3_5 */
			param_types = f_p_type->getArgTypes ();
#endif /* !HAVE_LLVM_3_5 */

			t = context.getFunctionType (new_result_type,
			                             param_types,
			                             f_p_type->getExtProtoInfo ());
		}

		DEBUG ("Constifying type " <<
		       func_decl->getType ().getAsString () << " → " <<
		       t.getAsString ());
		func_decl->setType (t);
	}
}

/* Determine whether the given function should be excluded from having extra
 * nonnull attributes added due to being GLib-internal. Such functions are
 * exported as symbols from libglib-2.0.so, but are deliberately not in the GIR
 * file — and hence we can’t pick up annotations on them. */
static bool
_ignore_glib_internal_func (const std::string func_name)
{
	static const char *internal_funcs[] = {
		"g_assertion_message",
		"g_assertion_message_cmpnum",
		"g_assertion_message_cmpstr",
		"g_assertion_message_error",
		"g_assertion_message_expr",
		"g_test_trap_assertions",
		"g_return_if_fail_warning",
		"g_warn_message",
	};

	for (unsigned int i = 0; i < G_N_ELEMENTS (internal_funcs); i++) {
		if (strcmp (func_name.c_str (), internal_funcs[i]) == 0) {
			return true;
		}
	}

	return false;
}

/**
 * AllocatorInfo:
 *
 * Store information about a data type, its allocation functions and free
 * functions, and what kinds of allocation systems it supports (e.g., reference
 * counting, explicit destruction, etc.).
 */
typedef struct AllocatorInfo {
	/* Identifier for the allocator and free functions, e.g. malloc() and
	 * free(). */
	const IdentifierInfo *allocator_identifier;
	const IdentifierInfo *free_identifier;

	/* Whether the allocator supports reference counting semantics. */
	bool supports_ref_counting;

	/* Whether the allocator supports instantly dropping all references for
	 * an allocation. */
	bool supports_free;

	AllocatorInfo () : allocator_identifier (NULL), free_identifier (NULL),
	                   supports_ref_counting (false),
	                   supports_free (false) {};
	AllocatorInfo (const IdentifierInfo *aid, const IdentifierInfo *fid,
	               bool src,
	               bool sf) : allocator_identifier (aid),
	                          free_identifier (fid),
	                          supports_ref_counting (src),
	                          supports_free (sf) {};
} AllocatorInfo;

/* Return a dummy AllocatorInfo for an unknown, non-allocated or unsupported
 * type. Return a useful AllocatorInfo containing the IdentifierInfo of the
 * allocator function associated with the type otherwise.
 *
 * Note that currently only types allocated using malloc() (which we consider
 * equivalent to g_malloc() and friends) are supported at the moment. */
static AllocatorInfo
_type_to_allocator_info (GITypeInfo *type_info, GITypeTag type_tag,
                         const std::string &func_name,
                         const ASTContext &ast_context)
{
	switch (type_tag) {
	case GI_TYPE_TAG_VOID:
	case GI_TYPE_TAG_BOOLEAN:
	case GI_TYPE_TAG_INT8:
	case GI_TYPE_TAG_UINT8:
	case GI_TYPE_TAG_INT16:
	case GI_TYPE_TAG_UINT16:
	case GI_TYPE_TAG_INT32:
	case GI_TYPE_TAG_UINT32:
	case GI_TYPE_TAG_INT64:
	case GI_TYPE_TAG_UINT64:
	case GI_TYPE_TAG_FLOAT:
	case GI_TYPE_TAG_DOUBLE:
	case GI_TYPE_TAG_UNICHAR:
		/* Non-allocated types. */
		return AllocatorInfo ();
	case GI_TYPE_TAG_GTYPE:
		/* Unsupported types. */
		return AllocatorInfo ();
	case GI_TYPE_TAG_UTF8:
	case GI_TYPE_TAG_FILENAME:
		/* Supported types. */
		return AllocatorInfo (&ast_context.Idents.get ("g_malloc"),
		                      &ast_context.Idents.get ("g_free"),
		                      false, true);
	case GI_TYPE_TAG_ARRAY:
		return AllocatorInfo (&ast_context.Idents.get ("g_ptr_array_new_full"),
		                      &ast_context.Idents.get ("g_ptr_array_unref"),
		                      true, true);
	case GI_TYPE_TAG_INTERFACE:
		return AllocatorInfo (&ast_context.Idents.get ("g_object_ref"),
		                      &ast_context.Idents.get ("g_object_unref"),
		                      true, false);
	case GI_TYPE_TAG_GLIST:
		return AllocatorInfo (&ast_context.Idents.get ("g_list_alloc"),
		                      &ast_context.Idents.get ("g_list_free_1"),
		                      false, true);
	case GI_TYPE_TAG_GSLIST:
		return AllocatorInfo (&ast_context.Idents.get ("g_slist_alloc"),
		                      &ast_context.Idents.get ("g_slist_free_1"),
		                      false, true);
	case GI_TYPE_TAG_GHASH:
		return AllocatorInfo (&ast_context.Idents.get ("g_hash_table_new_full"),
		                      &ast_context.Idents.get ("g_hash_table_unref"),
		                      true, true);
	case GI_TYPE_TAG_ERROR:
		return AllocatorInfo (&ast_context.Idents.get ("g_error_new"),
		                      &ast_context.Idents.get ("g_error_free"),
		                      false, true);
	default:
		WARN ("Error: Unhandled GI type tag " << type_tag <<
		      " in introspection info for function ‘" <<
		      func_name << "’.");
		return AllocatorInfo ();
	}
}

typedef struct {
	const gchar *func_name;

	bool ownership_returns;
	int ownership_returns_arg;

	int ownership_takes_arg;
	int ownership_holds_arg;
} NonGirInfo;

static const NonGirInfo _non_gir_info[] = {
	/* g_new(), g_new0(), g_renew(), g_try_new(), g_try_new0(),
	 * g_try_renew() are all macros. */
	/* malloc()-like functions: */
	{ "g_malloc", true, 0, -1, -1 },
	{ "g_malloc0", true, 0, -1, -1 },
	{ "g_try_malloc", true, 0, -1, -1 },
	{ "g_try_malloc0", true, 0, -1, -1 },
	/* calloc()-like function:
	 * FIXME: Add support for calloc()-like ownership_returns upstream. */
	{ "g_malloc_n", true, -1, -1, -1 },
	{ "g_malloc0_n", true, -1, -1, -1 },
	{ "g_try_malloc_n", true, -1, -1, -1 },
	{ "g_try_malloc0_n", true, -1, -1, -1 },
	/* realloc()-like functions: */
	{ "g_realloc", true, 1, 0, -1 },
	{ "g_try_realloc", true, 1, 0, -1 },
	{ "g_realloc_n", true, -1, 0, -1 },
	{ "g_try_realloc_n", true, -1, 0, -1 },
	/* free()-like functions: */
	{ "g_free", false, -1, 0, -1 },
	/* GObject ref-counting functions: */
	{ "g_object_ref", true, -1, -1, -1 },
	{ "g_object_unref", false, -1, 0, -1 },

	/* FIXME: Can’t support g_clear_pointer(), g_memdup(), g_clear_object(),
	 * floating references, weak references, toggle references. */

	/* Overrides which do exist in the GIR file but have to be here because
	 * memory management happens on their instance-parameters. See the
	 * FIXME below. */
	{ "g_string_free", true, -1, 0, -1 },
};

/* Return a NonGirInfo structure for this @decl if it’s a memory allocation
 * function which is non-introspectable. Return %NULL otherwise. */
static const NonGirInfo *
_is_non_gir_function (const FunctionDecl &decl)
{
	guint i;
	const std::string func_name = decl.getNameAsString ();

	for (i = 0; i < G_N_ELEMENTS (_non_gir_info); i++) {
		const NonGirInfo *info = &_non_gir_info[i];

		if (info->func_name == func_name) {
			return info;
		}
	}

	return NULL;
}

/* There are various memory management functions which are not exposed in GIR,
 * but which are vital for correct memory management. Manually add attributes to
 * them.
 *
 * FIXME: This is not ideal. They should somehow be exposed from the typelib.
 */
static void
_non_gir_func_process_ownership (FunctionDecl &decl,
                                 const NonGirInfo *info)
{
	/* Sanity check. */
	assert (!(info->ownership_takes_arg >= 0 &&
	          info->ownership_holds_arg >= 0 &&
	          info->ownership_takes_arg == info->ownership_holds_arg));

	/* FIXME: See earlier comment about having to use malloc(). */
	ASTContext &ast_context = decl.getASTContext ();
	IdentifierInfo &malloc_info = ast_context.Idents.get ("malloc");

	/* ownership_returns. */
	if (info->ownership_returns && info->ownership_returns_arg >= 0) {
		unsigned int arg = info->ownership_returns_arg;
		decl.addAttr (::new (ast_context)
		              OwnershipAttr (decl.getSourceRange (),
		                             ast_context, &malloc_info, &arg, 1,
		                             OwnershipAttr::Returns));
	} else if (info->ownership_returns) {
		decl.addAttr (::new (ast_context)
		              OwnershipAttr (decl.getSourceRange (),
		                             ast_context, &malloc_info, NULL, 0,
		                             OwnershipAttr::Returns));
	}

	/* ownership_holds. */
	if (info->ownership_holds_arg >= 0) {
		unsigned int arg = info->ownership_holds_arg;
		decl.addAttr (::new (ast_context)
		              OwnershipAttr (decl.getSourceRange (),
		                             ast_context, &malloc_info, &arg, 1,
		                             OwnershipAttr::Holds));
	}

	/* ownership_takes. */
	if (info->ownership_takes_arg >= 0) {
		unsigned int arg = info->ownership_takes_arg;
		decl.addAttr (::new (ast_context)
		              OwnershipAttr (decl.getSourceRange (),
		                             ast_context, &malloc_info, &arg, 1,
		                             OwnershipAttr::Takes));
	}
}

/**
 * _arg_process_ownership:
 *
 * Examine various pieces of information about a function argument and return
 * either a new #OwnershipAttr attribute to represent the ownership transfer
 * into or out of that argument; or %NULL to indicate no ownership transfer or
 * unknown transfer.
 *
 * FIXME: This cannot currently represent %GI_TRANSFER_CONTAINER. It also cannot
 * model custom functions with obvious ownership transfer (even the simplest
 * wrapper around g_free(), for example). That should be implemented as a
 * separate checker so it can be used for all ownership sources.
 *
 * FIXME: Check for conflicts with existing ownership attributes.
 */
static OwnershipAttr *
_arg_process_ownership (GIArgInfo *arg, GITypeInfo *type_info,
                        GITransfer transfer, GIDirection direction,
                        GITypeTag type_tag, const std::string &func_name,
                        SourceRange source_range, unsigned int arg_index,
                        ASTContext &ast_context)
{
	/* Check the transfer type. */
	switch (transfer) {
	case GI_TRANSFER_NOTHING:
		/* No ownership attribute needed. */
		return NULL;
	case GI_TRANSFER_CONTAINER:
	case GI_TRANSFER_EVERYTHING:
		/* Cannot currently differentiate between these. Both are
		 * supported. */
		break;
	default:
		WARN ("Error: Unhandled GI transfer " << transfer << " in "
		      "introspection info for function ‘" << func_name << "’.");
		return NULL;
	}

	/* Check the direction. */
	switch (direction) {
	case GI_DIRECTION_IN:
	case GI_DIRECTION_INOUT:
		/* These are supported. */
		break;
	case GI_DIRECTION_OUT:
		/* ownership_returns doesn’t support out-parameters. */
		return NULL;
	default:
		WARN ("Error: Unhandled GI direction " << direction << " in "
		      "introspection info for function ‘" << func_name << "’.");
		return NULL;
	}

	/* Grab the allocator info. */
	const AllocatorInfo allocator_info =
		_type_to_allocator_info (type_info, type_tag, func_name,
		                         ast_context);

	/* If the argument is suitable, create an OwnershipAttr for it. */
	int attr_spelling_index = -1;  /* no attribute */
	bool is_pessimistic = true;  /* FIXME: turn this into an option */

	if (allocator_info.supports_ref_counting &&
	    !allocator_info.supports_free) {
		attr_spelling_index = OwnershipAttr::Holds;
	} else if (!allocator_info.supports_ref_counting &&
	           allocator_info.supports_free) {
		attr_spelling_index = OwnershipAttr::Takes;
	} else if (allocator_info.supports_ref_counting &&
	           allocator_info.supports_free) {
		/* This is for the case of allocation functions like
		 * g_ptr_array_free(), where their behaviour is controlled by an
		 * argument to the function, deciding whether it frees
		 * everything or only some things. */
		attr_spelling_index =
			is_pessimistic ? OwnershipAttr::Holds : OwnershipAttr::Takes;
	}

	/* FIXME: The MallocChecker is only good for malloc() functions at the
	 * moment. So use malloc() as the identifier for everything so that we
	 * get some warnings rather than none. In future, use
	 * allocator_info.allocator_identifier instead, so we can pair
	 * alloc/free functions as well. */
	IdentifierInfo &malloc_info = ast_context.Idents.get ("malloc");

	if (attr_spelling_index != -1) {
		return ::new (ast_context)
		       OwnershipAttr (source_range, ast_context, &malloc_info,
		                      &arg_index, 1, attr_spelling_index);
	}

	return NULL;
}

/**
 * _return_process_ownership:
 *
 * Similar to _arg_process_ownership(), but for function return values.
 */
static OwnershipAttr *
_return_process_ownership (GITypeInfo *type_info, GITransfer transfer,
                           GITypeTag type_tag, const std::string &func_name,
                           SourceRange source_range, ASTContext &ast_context)
{
	/* Check the transfer type. */
	switch (transfer) {
	case GI_TRANSFER_NOTHING:
		/* No ownership attribute needed. */
		return NULL;
	case GI_TRANSFER_CONTAINER:
	case GI_TRANSFER_EVERYTHING:
		/* Cannot currently differentiate between these. Both are
		 * supported. */
		break;
	default:
		WARN ("Error: Unhandled GI transfer " << transfer << " in "
		      "introspection info for function ‘" << func_name << "’.");
		return NULL;
	}

	/* Grab the allocator info. */
	const AllocatorInfo allocator_info =
		_type_to_allocator_info (type_info, type_tag, func_name,
		                         ast_context);

	/* If the argument is suitable, create an OwnershipAttr for it. */
	if (allocator_info.supports_ref_counting ||
	    allocator_info.supports_free) {
		/* FIXME: See earlier comment about having to use malloc(). */
		IdentifierInfo &malloc_info = ast_context.Idents.get ("malloc");

		return ::new (ast_context)
		       OwnershipAttr (source_range, ast_context, &malloc_info,
		                      NULL, 0, OwnershipAttr::Returns);
	}

	return NULL;
}

void
GirAttributesConsumer::_handle_function_decl (FunctionDecl& func)
{
	/* Ignore static functions immediately; they shouldn’t have any
	 * GIR data, and searching for it massively slows down
	 * compilation. */
	StorageClass sc = func.getStorageClass ();
	if (sc != SC_None && sc != SC_Extern)
		return;

	/* Try to find typelib or override information about the function.
	 *
	 * FIXME: We have to search for override information even if we have GIR
	 * information because of functions like g_string_free(). They are in
	 * the GIR file as methods, meaning their instance-parameter is not
	 * exposed as a GIArgInfo, meaning we cannot use the code below on it.
	 * Ideally, we could expose the instance-parameter too, and extra its
	 * transfer information as any other arg.
	 */
	const std::string func_name = func.getNameAsString ();
	GIBaseInfo *info = this->_gir_manager.get ()->find_function_info (func_name);
	const NonGirInfo *non_gir_info = _is_non_gir_function (func);

	if (non_gir_info != NULL) {
		_non_gir_func_process_ownership (func, non_gir_info);
		return;
	}

	if (info == NULL) {
		return;
	}

	/* Extract information from the GIBaseInfo and add AST attributes
	 * accordingly. */
	switch (g_base_info_get_type (info)) {
	case GI_INFO_TYPE_FUNCTION: {
		GICallableInfo *callable_info = (GICallableInfo *) info;

		/* GError formal parameters aren’t included in the number of
		 * callable arguments. */
		unsigned int k = g_callable_info_get_n_args (callable_info);
		unsigned int self_params =
			(g_function_info_get_flags (callable_info) &
			 GI_FUNCTION_IS_METHOD) ? 1 : 0;
		unsigned int err_params =
			(g_function_info_get_flags (callable_info) &
			 GI_FUNCTION_THROWS) ? 1 : 0;

		/* Sanity check. */
		if (k + self_params + err_params != func.getNumParams ()) {
			WARN ("Number of GIR callable parameters (" << k << ") "
			      "differs from number of C formal parameters (" <<
			      func.getNumParams () << "). Ignoring function " <<
			      func_name << "().");
			break;
		}

		std::vector<unsigned int> non_null_args;
		unsigned int j;

		NonNullAttr* nonnull_attr = func.getAttr<NonNullAttr> ();
		if (nonnull_attr != NULL) {
			/* Extend and replace the existing attribute. */
			DEBUG ("Extending existing attribute.");
			non_null_args.insert (non_null_args.begin (),
			                      nonnull_attr->args_begin (),
			                      nonnull_attr->args_end ());
		}

		for (j = self_params; j < k + self_params; j++) {
			GIArgInfo arg;
			GITypeInfo type_info;
			GITransfer transfer;
			GIDirection direction;
			GITypeTag type_tag;

			g_callable_info_load_arg (callable_info,
			                          j - self_params, &arg);
			g_arg_info_load_type (&arg, &type_info);
			transfer = g_arg_info_get_ownership_transfer (&arg);
			direction = g_arg_info_get_direction (&arg);
			type_tag = g_type_info_get_tag (&type_info);

			int array_type =
				(g_type_info_get_tag (&type_info) ==
				 GI_TYPE_TAG_ARRAY) ?
					g_type_info_get_array_type (&type_info) :
					-1;
			DEBUG ("GirAttributes: " << func_name << "(" << j <<
			       ")\n"
			       "\tTransfer: " << transfer << "\n"
			       "\tDirection: " << direction << "\n"
			       "\tNullable: " <<
			       g_arg_info_may_be_null (&arg) << "\n"
			       "\tOptional: " <<
			       g_arg_info_is_optional (&arg) << "\n"
			       "\tIs pointer: " <<
			       g_type_info_is_pointer (&type_info) << "\n"
			       "\tType tag: " <<
			       g_type_tag_to_string (type_tag) << "\n"
			       "\tArray type: " <<
			       array_type << "\n"
			       "\tArray length: " <<
			       g_type_info_get_array_length (&type_info) << "\n"
			       "\tArray fixed size: " <<
			       g_type_info_get_array_fixed_size (&type_info));

			if (_arg_is_nonnull (arg, type_info)) {
				DEBUG ("Got nonnull arg " << j << " from GIR.");
				non_null_args.push_back (j);
			}

			/* Ownership. */
			OwnershipAttr *ownership_attr;

			ownership_attr = _arg_process_ownership (&arg,
			                                         &type_info,
			                                         transfer,
			                                         direction,
			                                         type_tag,
			                                         func_name,
			                                         func.getSourceRange (),
			                                         j,
			                                         func.getASTContext ());

			if (ownership_attr != NULL) {
				func.addAttr (ownership_attr);
			}

			if (_type_should_be_const (transfer, type_tag)) {
				ParmVarDecl *parm = func.getParamDecl (j);
				QualType t = parm->getType ();

				if (!t.isConstant (parm->getASTContext ()))
					parm->setType (t.withConst ());
			}
		}

		if (non_null_args.size () > 0 &&
		    !_ignore_glib_internal_func (func_name)) {
#ifdef HAVE_LLVM_3_5
			nonnull_attr = ::new (func.getASTContext ())
				NonNullAttr (func.getSourceRange (),
				             func.getASTContext (),
				             non_null_args.data (),
				             non_null_args.size (), 0);
#else /* if !HAVE_LLVM_3_5 */
			nonnull_attr = ::new (func.getASTContext ())
				NonNullAttr (func.getSourceRange (),
				             func.getASTContext (),
				             non_null_args.data (),
				             non_null_args.size ());
#endif /* !HAVE_LLVM_3_5 */
			func.addAttr (nonnull_attr);
		}

		/* Process the function’s return type. */
		/* FIXME: Support returns_nonnull when Clang supports it.
		 * http://llvm.org/bugs/show_bug.cgi?id=4832 */
		GITypeInfo return_type_info;
		GITransfer return_transfer;
		GITypeTag return_type_tag;

		g_callable_info_load_return_type (info, &return_type_info);
		return_transfer = g_callable_info_get_caller_owns (info);
		return_type_tag = g_type_info_get_tag (&return_type_info);

		if (return_transfer != GI_TRANSFER_NOTHING) {
#ifdef HAVE_LLVM_3_5
			WarnUnusedAttr* warn_unused_attr =
				::new (func.getASTContext ())
				WarnUnusedAttr (func.getSourceRange (),
				                func.getASTContext (), 0);
#else /* if !HAVE_LLVM_3_5 */
			WarnUnusedAttr* warn_unused_attr =
				::new (func.getASTContext ())
				WarnUnusedAttr (func.getSourceRange (),
				                func.getASTContext ());
#endif /* !HAVE_LLVM_3_5 */
			func.addAttr (warn_unused_attr);
		} else if (_type_should_be_const (return_transfer,
		                                  return_type_tag)) {
			_constify_function_return_type (func);
		}

		/* ownership_returns. */
		OwnershipAttr *ownership_attr =
			_return_process_ownership (&return_type_info,
			                           return_transfer,
			                           return_type_tag, func_name,
			                           func.getSourceRange (),
			                           func.getASTContext ());

		if (ownership_attr != NULL) {
			func.addAttr (ownership_attr);
		}

		/* Mark the function as deprecated if it wasn’t already. The
		 * typelib file doesn’t contain a deprecation message, version,
		 * or replacement function so we can’t make use of them. */
		if (g_base_info_is_deprecated (info) &&
		    !func.hasAttr<DeprecatedAttr> ()) {
#ifdef HAVE_LLVM_3_5
			DeprecatedAttr* deprecated_attr =
				::new (func.getASTContext ())
				DeprecatedAttr (func.getSourceRange (),
				                func.getASTContext (),
				                "Deprecated using the gtk-doc "
				                "attribute.", 0);
#else /* if !HAVE_LLVM_3_5 */
			DeprecatedAttr* deprecated_attr =
				::new (func.getASTContext ())
				DeprecatedAttr (func.getSourceRange (),
				                func.getASTContext (),
				                "Deprecated using the gtk-doc "
				                "attribute.");
#endif /* !HAVE_LLVM_3_5 */
			func.addAttr (deprecated_attr);
		}

		/* Mark the function as allocating memory if it’s a
		 * constructor. Do not add an ownership_returns attribute here:
		 * it should have already been handled by the (transfer)
		 * annotation of the return value. */
		if (g_function_info_get_flags (info) &
		    GI_FUNCTION_IS_CONSTRUCTOR &&
		    !func.hasAttr<MallocAttr> ()) {
#ifdef HAVE_LLVM_3_5
			MallocAttr* malloc_attr =
				::new (func.getASTContext ())
				MallocAttr (func.getSourceRange (),
				            func.getASTContext (), 0);
#else /* if !HAVE_LLVM_3_5 */
			MallocAttr* malloc_attr =
				::new (func.getASTContext ())
				MallocAttr (func.getSourceRange (),
				            func.getASTContext ());
#endif /* !HAVE_LLVM_3_5 */
			func.addAttr (malloc_attr);
		}

		break;
	}
	case GI_INFO_TYPE_CALLBACK:
	case GI_INFO_TYPE_STRUCT:
	case GI_INFO_TYPE_BOXED:
	case GI_INFO_TYPE_ENUM:
	case GI_INFO_TYPE_FLAGS:
	case GI_INFO_TYPE_OBJECT:
	case GI_INFO_TYPE_INTERFACE:
	case GI_INFO_TYPE_CONSTANT:
	case GI_INFO_TYPE_INVALID_0:
	case GI_INFO_TYPE_UNION:
	case GI_INFO_TYPE_VALUE:
	case GI_INFO_TYPE_SIGNAL:
	case GI_INFO_TYPE_VFUNC:
	case GI_INFO_TYPE_PROPERTY:
	case GI_INFO_TYPE_FIELD:
	case GI_INFO_TYPE_ARG:
	case GI_INFO_TYPE_TYPE:
	case GI_INFO_TYPE_UNRESOLVED:
	case GI_INFO_TYPE_INVALID:
	default:
		WARN ("Error: Unhandled GI type " <<
		      g_base_info_get_type (info) << " in introspection info "
		      "for function ‘" << func_name << "’.");
	}

	g_base_info_unref (info);
}

bool
GirAttributesConsumer::HandleTopLevelDecl (DeclGroupRef decl_group)
{
	DeclGroupRef::iterator i, e;

	for (i = decl_group.begin (), e = decl_group.end (); i != e; i++) {
		Decl *decl = *i;
		FunctionDecl *func = dyn_cast<FunctionDecl> (decl);

		/* We’re only interested in function declarations. */
		if (func == NULL)
			continue;

		this->_handle_function_decl (*func);
	}

	return true;
}


void
GirAttributesChecker::_handle_function_decl (FunctionDecl& func)
{
	/* Ignore static functions immediately; they shouldn’t have any
	 * GIR data, and searching for it massively slows down
	 * compilation. */
	StorageClass sc = func.getStorageClass ();
	if (sc != SC_None && sc != SC_Extern)
		return;

	/* Try to find typelib information about the function. */
	const std::string func_name = func.getNameAsString ();
	GIBaseInfo *info = this->_gir_manager.get ()->find_function_info (func_name);

	if (info == NULL)
		return;

	/* Extract information from the GIBaseInfo and check AST attributes
	 * accordingly. */
	switch (g_base_info_get_type (info)) {
	case GI_INFO_TYPE_FUNCTION: {
		GICallableInfo *callable_info = (GICallableInfo *) info;

		/* GError formal parameters aren’t included in the number of
		 * callable arguments. */
		unsigned int k = g_callable_info_get_n_args (callable_info);
		unsigned int self_params =
			(g_function_info_get_flags (callable_info) &
			 GI_FUNCTION_IS_METHOD) ? 1 : 0;
		unsigned int err_params =
			(g_function_info_get_flags (callable_info) &
			 GI_FUNCTION_THROWS) ? 1 : 0;

		/* Sanity check. */
		if (k + self_params + err_params != func.getNumParams ()) {
			WARN ("Number of GIR callable parameters (" << k << ") "
			      "differs from number of C formal parameters (" <<
			      func.getNumParams () << "). Ignoring function " <<
			      func_name << "().");
			break;
		}

		/* Process the function’s return type. */
		GITypeInfo return_type_info;
		GITransfer return_transfer;
		GITypeTag return_type_tag;

		g_callable_info_load_return_type (info, &return_type_info);
		return_transfer = g_callable_info_get_caller_owns (info);
		return_type_tag = g_type_info_get_tag (&return_type_info);

		/* If the return type is const-qualified but no (transfer none)
		 * annotation exists, emit a warning.
		 *
		 * Similarly, if a (transfer none) annotation exists but the
		 * return type is not const-qualified, emit a warning. */
		if (_function_return_type_is_const (func) &&
		    return_transfer != GI_TRANSFER_NOTHING) {
			Debug::emit_error (
				"Missing (transfer none) annotation on the "
				"return value of function %0() (already has a "
				"const modifier).",
				this->_compiler,
				func.getLocStart ())
			<< func.getNameAsString ();
		} else if (return_transfer == GI_TRANSFER_NOTHING &&
		           _type_should_be_const (return_transfer,
		                                  return_type_tag) &&
		           !_function_return_type_is_const (func)) {
			Debug::emit_error (
				"Missing const modifier on the return value of "
				"function %0() (already has a (transfer none) "
				"annotation).",
				this->_compiler,
				func.getLocStart ())
			<< func.getNameAsString ();
		}

		break;
	}
	case GI_INFO_TYPE_CALLBACK:
	case GI_INFO_TYPE_STRUCT:
	case GI_INFO_TYPE_BOXED:
	case GI_INFO_TYPE_ENUM:
	case GI_INFO_TYPE_FLAGS:
	case GI_INFO_TYPE_OBJECT:
	case GI_INFO_TYPE_INTERFACE:
	case GI_INFO_TYPE_CONSTANT:
	case GI_INFO_TYPE_INVALID_0:
	case GI_INFO_TYPE_UNION:
	case GI_INFO_TYPE_VALUE:
	case GI_INFO_TYPE_SIGNAL:
	case GI_INFO_TYPE_VFUNC:
	case GI_INFO_TYPE_PROPERTY:
	case GI_INFO_TYPE_FIELD:
	case GI_INFO_TYPE_ARG:
	case GI_INFO_TYPE_TYPE:
	case GI_INFO_TYPE_UNRESOLVED:
	case GI_INFO_TYPE_INVALID:
	default:
		WARN ("Error: Unhandled GI type " <<
		      g_base_info_get_type (info) << " in introspection info "
		      "for function ‘" << func_name << "’.");
	}

	g_base_info_unref (info);
}

bool
GirAttributesChecker::HandleTopLevelDecl (DeclGroupRef decl_group)
{
	DeclGroupRef::iterator i, e;

	/* Run away if the plugin is disabled. */
	if (!this->is_enabled ()) {
		return true;
	}

	for (i = decl_group.begin (), e = decl_group.end (); i != e; i++) {
		Decl *decl = *i;
		FunctionDecl *func = dyn_cast<FunctionDecl> (decl);

		/* We’re only interested in function declarations. */
		if (func == NULL)
			continue;

		this->_handle_function_decl (*func);
	}

	return true;
}

} /* namespace tartan */
