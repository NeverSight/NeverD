import fixture.*;
public final class GuardedTypedDefaultsCaller {
    public static Child build(Object token) { return new Child(token); }
    public static Unit read(Child value,Object input) { return value.read(input); }
}
