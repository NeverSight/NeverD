package fixture;
import java.util.*;
@SuppressWarnings({"rawtypes","unchecked"})
public final class Core {
    private Core() {}
    public static Object direct(Collection input) {return Factory.select(input);}
    public static Comparable checked(Collection input) {return (Comparable)Factory.select(input);}
    public static <E extends Object & Comparable<? super E>> E relay(Collection<? extends E> input) {return Factory.again(input);}
    public static Object sdk(Collection input) {return Collections.max(input);}
}
