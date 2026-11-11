package fixture;

public final class Core {
    private Core() {}
    public static Object[] raw(Object source, Class<Object> token, java.util.function.Function<Object,Object> mapper) {
        return Provider.read(source, token, mapper);
    }
    public static String[] narrow(Object source, Class<Object> token, java.util.function.Function<Object,Object> mapper) {
        return (String[])Provider.read(source, token, mapper);
    }
    public static Object ignored(Object source, Class<Object> token, java.util.function.Function<Object,Object> mapper) {
        Provider.read(source, token, mapper);
        return Trace.marker;
    }
    public static Object[] relay(Object source, Class<Object> token, java.util.function.Function<Object,Object> mapper) {
        return Provider.relay(source, token, mapper);
    }
    public static <T,R> R[] typed(Object source, Class<R> token, java.util.function.Function<? super T,? extends R> mapper) {
        return Provider.read(source, token, mapper);
    }
    @SuppressWarnings("unchecked")
    public static Object[] token(Object source, Class<?> token, java.util.function.Function<Object,Object> mapper) {
        return Provider.read(source, (Class<Object>)token, mapper);
    }
    public static Object[] alias(Object source, Class<Object> token, java.util.function.Function<Object,Object> mapper) {
        return Facade.read(source, token, mapper);
    }
}
