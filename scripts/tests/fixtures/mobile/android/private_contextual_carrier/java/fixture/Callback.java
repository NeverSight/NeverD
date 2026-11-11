package fixture;
public interface Callback<E> { Carrier<E> create(Context context, int index, Carrier<E> seed); }
