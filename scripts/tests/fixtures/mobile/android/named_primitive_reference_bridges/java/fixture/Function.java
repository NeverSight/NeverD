package fixture;
public interface Function<T, V extends Bound> {
    Object apply(Context context, T first, T second, Callback<? super T> callback, Box<T, V> box);
}
