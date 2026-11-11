package fixture;

public interface ScopeRoot {
    Object context();
    default Object identity(Object value) {
        Trace.mark(1);
        Object context = context();
        Trace.mark(2);
        return context == value ? context : value;
    }
}
