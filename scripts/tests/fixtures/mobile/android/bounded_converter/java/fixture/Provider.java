package fixture;
public final class Provider {
    public static Bound replacement;
    private static <T,V extends Bound> V convert(Converter<T,V> converter,T value) {
        if(value==null)return null;
        return converter.get().apply(value);
    }
    @SuppressWarnings("unchecked")
    public static Bound access(Converter converter,Object value) { return convert(converter,value); }
}
