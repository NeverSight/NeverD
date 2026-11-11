package fixture;
public interface Api extends Parent, Comparable<Api> {
    Api shift(long delta);
    int compareTo(Api other);
}
