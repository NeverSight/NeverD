package fixture;
public interface API {
    <I, O> Box<I> make(Transform<I, O> transform, Callback<O> callback, long flags);
}
