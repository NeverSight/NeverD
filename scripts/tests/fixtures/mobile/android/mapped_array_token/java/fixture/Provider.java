package fixture;

public class Provider {
    public Provider() {}
    @SuppressWarnings("unchecked")
    public static <T,R> R[] read(Object source, Class<R> token, java.util.function.Function<? super T,? extends R> mapper) {
        Trace.calls++;
        Trace.stage = 1;
        Trace.source = source;
        Trace.token = token;
        Trace.mapper = mapper;
        Trace.name = token.getName();
        Trace.stage = 2;
        R mapped = mapper.apply((T)source);
        Trace.mapped = mapped;
        Trace.stage = 3;
        Trace.action.run();
        Trace.stage = 4;
        return (R[])Trace.value;
    }
    public static <T,R> R[] relay(Object source, Class<R> token, java.util.function.Function<? super T,? extends R> mapper) {
        Trace.relays++;
        return read(source, token, mapper);
    }
}
