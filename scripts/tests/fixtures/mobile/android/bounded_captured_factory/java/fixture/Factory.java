package fixture;
public class Factory {
    public static <S,T,V extends Vector> Outer<S>.Member<T,V> create(
            Outer<S> outer, Converter<T,V> converter, String label, int mode) {
        Trace.hit(1);
        T value = converter.value();
        V vector = converter.convert(value);
        Trace.hit(4);
        Outer<S> chosen = mode == 0 ? outer : outer.destination;
        Outer<S>.Member<T,V> result = chosen.new Member<T,V>(value, vector);
        Trace.last = result;
        Trace.hit(5);
        return result;
    }
}
