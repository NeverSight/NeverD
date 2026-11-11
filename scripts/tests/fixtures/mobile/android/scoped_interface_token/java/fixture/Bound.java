package fixture;
public class Bound implements java.lang.annotation.Annotation {
    public Object value;
    public Class<? extends java.lang.annotation.Annotation> annotationType() { return Deprecated.class; }
}
