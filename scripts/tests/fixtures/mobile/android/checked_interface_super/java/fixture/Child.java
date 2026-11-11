package fixture;

import java.util.Collection;
import java.util.Set;

public final class Child<E> extends Parent<E> {
    static { Trace.hit(9); }

    @SuppressWarnings("unchecked")
    public Child(Set<? extends E> input) {
        super((Collection<? extends E>)(Object)input);
        Trace.hit(3);
    }
}
