package fixture;
import java.util.function.Function;
public final class Core {
    private Core() {}
    public static Object raw() {return Mirror.build();}
    public static Token checked() {return (Token)(Object)Mirror.<Object>build();}
    public static Object ignored() {Mirror.build();return Trace.marker;}
    public static <E> Mirror<E,E> relay() {return Mirror.build();}
    public static Object standard() {return java.util.function.Function.identity();}
    public static String applied(Object value) {return (String)Function.<Object>identity().apply(value);}
}
