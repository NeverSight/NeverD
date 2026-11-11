package fixture;
public class Caller {
    public Caller() {}
    public static <T> Base<T> readWide(Ancestor<T> value) { return value.items(); }
    public static <T> Leaf<T> readNarrow(Api<T> value) { return value.narrowItems(); }
}
