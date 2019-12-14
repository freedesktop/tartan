/* Template: assertion */

/*
 * null passed to a callee that requires a non-null argument
 *         assertion_func (NULL, 1, obj);
 *                         ~~~~        ^
 * null passed to a callee that requires a non-null argument
 *         assertion_func (NULL, 3, NULL);
 *                         ~~~~         ^
 */
{
    struct Temp {
        operator bool () { return true; }
        ~Temp () {}
    };
    g_return_if_fail (Temp () && some_str);
}
