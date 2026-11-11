package fixture;
import java.util.*;
public class Factory {
    public static int calls;
    public static Collection last;
    public static <E extends Object & Comparable<? super E>> E select(Collection<? extends E> input) {
        calls++;last=input;return Collections.max(input);
    }
    public static <E extends Object & Comparable<? super E>> E again(Collection<? extends E> input) {
        return select(input);
    }
}
