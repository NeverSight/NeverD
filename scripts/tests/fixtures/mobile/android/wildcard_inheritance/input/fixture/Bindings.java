package fixture;

import fixture.dependency.ChildTag;
import fixture.dependency.Contravariant;
import fixture.dependency.Covariant;
import fixture.dependency.Leaf;
import fixture.dependency.Tag;
import fixture.dependency.Unbounded;

public final class Bindings {
    public static Covariant<Leaf<? extends ChildTag>> upper;
    public static Contravariant<Leaf<? super Tag>> lower;
    public static Unbounded<Leaf<?>> any;
    public static Covariant<Leaf<? extends ChildTag>> chooseUpper(boolean first,
            Covariant<Leaf<? extends ChildTag>> left, Covariant<Leaf<? extends ChildTag>> right) {
        return first ? left : right;
    }
    public static Contravariant<Leaf<? super Tag>> chooseLower(boolean first,
            Contravariant<Leaf<? super Tag>> left, Contravariant<Leaf<? super Tag>> right) {
        return first ? left : right;
    }
    public static Unbounded<Leaf<?>> chooseAny(boolean first,
            Unbounded<Leaf<?>> left, Unbounded<Leaf<?>> right) {
        return first ? left : right;
    }
}
