/* Template: generic */

/*
 * warning: Potential memory leak
 * }
 * ^
 */
{
	// Sanity check that we have specified
	// -analyzer-checker=alpha.unix.MallocWithAnnotations
	malloc (15);
}

/*
 * warning: Potential leak of memory pointed to by 'str'
 * }
 * ^
 */
{
	GString *str = g_string_new ("");
	printf ("%p\n", str);
}

/*
 * warning: Potential leak of memory pointed to by 'str'
 * }
 * ^
 */
{
	GString *str = g_string_new ("");
	printf ("%p\n", str);
	str = NULL;
}

/*
 * No error
 */
{
	GString *str = g_string_new ("");
	printf ("%p\n", str);
	g_string_free (str, TRUE);
}

/*
 * warning: Potential leak of memory pointed to by 'str'
 * }
 * ^
 */
{
	GString *str = g_string_new ("");

	// Something path-dependent.
	if (rand ()) {
		printf ("%p\n", str);
	} else {
		g_string_free (str, TRUE);
	}
}

/*
 * warning: Potential leak of memory pointed to by 'obj'
 * }
 * ^
 */
{
	GMountOperation *obj = g_mount_operation_new ();
	printf ("%p\n", obj);
}

/*
 * No error
 */
{
	GMountOperation *obj = g_mount_operation_new ();
	printf ("%p\n", obj);
	g_object_unref (obj);
}

/*
 * No error
 */
{
	GMountOperation *obj = g_mount_operation_new ();
	printf ("%p\n", obj);
	// FIXME: Unfortunately, since arbitrary functions aren’t modelled and
	// there’s no way to inject a non-ownership attribute into the
	// MallocChecker, opaque functions cause pointers to escape.
	// Clang needs a better representation of transfer than the ownership
	// attributes.
	g_mount_operation_set_username (obj, "blah");
}
