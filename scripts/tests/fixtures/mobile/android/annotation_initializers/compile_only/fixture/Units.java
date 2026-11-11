package fixture;

// Compile Trace's field access as getstatic. This declaration is never packaged
// into the DEX or present on the baseline/recovered runtime classpaths.
public @interface Units {
    int Z_VALUE = null == null ? 0 : 0;
}
