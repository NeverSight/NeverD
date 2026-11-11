package fixture;
public interface Function<P,Q,S,T,R> {
    R invoke(P flag, Q key, S previous, T replacement);
}
