package fixture;

import java.util.Collection;
import java.util.Set;

public class Parent<E> {
    static { Trace.hit(8); }
    public final Collection<? extends E> impl;

    public Parent(Collection<? extends E> input) {
        Trace.seen = input;
        Trace.hit(1);
        this.impl = input;
        Trace.owner = this;
        Trace.hit(2);
    }

    public Parent(Set<? extends E> input) {
        this.impl = input;
        Trace.wrong = true;
        Trace.hit(7);
    }

    public Parent(Object input) {
        this.impl = null;
        Trace.wrong = true;
        Trace.hit(6);
    }
}
