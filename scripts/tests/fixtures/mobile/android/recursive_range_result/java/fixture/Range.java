package fixture;
public interface Range<T extends Comparable<? super T>> { T lower(); T upper(); }
