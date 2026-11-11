import fixture.Trace;
import fixture.Units;

public class AnnotationInitializersHarness {
    @Units
    static class Marked {}

    public static void main(String[] args) throws Throwable {
        if (Trace.count != 0) throw new AssertionError("Annotation initialized too early");
        int value = Trace.read();
        if (Trace.count != 1 || value != 37)
            throw new AssertionError("Static field access did not retain initialization effects");
        if (Units.COMPANION != Units.Companion.INSTANCE || Units.COMPANION.read() != 37)
            throw new AssertionError("Annotation initialization cycle changed values or identity");
        if (!Units.class.isAnnotation() || Units.class.getDeclaredMethods().length != 1
                || Marked.class.getAnnotation(Units.class).value() != 37)
            throw new AssertionError("Annotation declaration or default changed");
        for (java.lang.reflect.Field field : Units.class.getDeclaredFields()) {
            int flags = field.getModifiers();
            if (!java.lang.reflect.Modifier.isPublic(flags) || !java.lang.reflect.Modifier.isStatic(flags)
                    || !java.lang.reflect.Modifier.isFinal(flags))
                throw new AssertionError("Annotation field modifiers changed");
        }
        System.out.println("initialization-count=" + Trace.count);
        System.out.println("cycle-value=" + Units.COMPANION.read());
        System.out.println("annotation-default=" + Marked.class.getAnnotation(Units.class).value());
        System.out.println("identity-and-modifiers=preserved");
    }
}
